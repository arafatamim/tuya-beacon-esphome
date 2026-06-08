package com.example.tuyabeaconforge

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.bluetooth.le.AdvertisingSet
import android.bluetooth.le.AdvertisingSetCallback
import android.bluetooth.le.AdvertisingSetParameters
import android.bluetooth.le.BluetoothLeAdvertiser
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.example.tuyabeaconforge.theme.TuyaBeaconForgeTheme
import java.util.UUID

// ---------------------------------------------------------------------------
// Forger: XXTEA(big-endian) + CRC-8 + Tuya-beacon DOWNLOAD_DP framing.
// Key = your bulb's localKey as 16 raw ASCII bytes (NOT hex-decoded). It rotates
// on every re-pair; extract it locally (see frida/ in the repo root).
// ---------------------------------------------------------------------------
object Forge {
    // TODO: put your device's 16-char localKey here.
    val KEY: ByteArray = "YOURLOCALKEY1234".toByteArray(Charsets.US_ASCII)
    private const val DELTA = -0x61c88647 // 0x9E3779B9

    private fun toWords(b: ByteArray): IntArray {
        val v = IntArray(4)
        for (i in 0 until 4) {
            v[i] = ((b[i * 4].toInt() and 0xff) shl 24) or
                ((b[i * 4 + 1].toInt() and 0xff) shl 16) or
                ((b[i * 4 + 2].toInt() and 0xff) shl 8) or
                (b[i * 4 + 3].toInt() and 0xff)
        }
        return v
    }

    private fun toBytes(v: IntArray): ByteArray {
        val b = ByteArray(16)
        for (i in 0 until 4) {
            b[i * 4] = (v[i] ushr 24).toByte()
            b[i * 4 + 1] = (v[i] ushr 16).toByte()
            b[i * 4 + 2] = (v[i] ushr 8).toByte()
            b[i * 4 + 3] = v[i].toByte()
        }
        return b
    }

    fun xxteaEnc(block: ByteArray): ByteArray {
        val k = toWords(KEY); val v = toWords(block); val n = 4
        var sum = 0; var z = v[n - 1]
        repeat(6 + 52 / n) {
            sum += DELTA
            val e = (sum ushr 2) and 3
            for (p in 0 until n) {
                val y = v[(p + 1) % n]
                val mx = (((z ushr 5) xor (y shl 2)) + ((y ushr 3) xor (z shl 4))) xor
                    ((sum xor y) + (k[(p and 3) xor e] xor z))
                v[p] += mx
                z = v[p]
            }
        }
        return toBytes(v)
    }

    fun xxteaDec(block: ByteArray): ByteArray {
        val k = toWords(KEY); val v = toWords(block); val n = 4
        var sum = (6 + 52 / n) * DELTA; var y = v[0]
        repeat(6 + 52 / n) {
            val e = (sum ushr 2) and 3
            for (p in n - 1 downTo 0) {
                val z = v[(p - 1 + n) % n]
                val mx = (((z ushr 5) xor (y shl 2)) + ((y ushr 3) xor (z shl 4))) xor
                    ((sum xor y) + (k[(p and 3) xor e] xor z))
                v[p] -= mx
                y = v[p]
            }
            sum -= DELTA
        }
        return toBytes(v)
    }

    /** If [onair16] is a valid Tuya command (op 03/06/07, marker 0x24), return its
     *  24-bit anti-replay counter, else -1. */
    fun counterOf(onair16: ByteArray): Int {
        if (onair16.size != 16) return -1
        val pt = xxteaDec(onair16)
        val op = pt[0].toInt() and 0xff
        if ((op == 0x03 || op == 0x06 || op == 0x07) && (pt[1].toInt() and 0xff) == 0x24) {
            return ((pt[2].toInt() and 0xff) shl 16) or ((pt[3].toInt() and 0xff) shl 8) or (pt[4].toInt() and 0xff)
        }
        return -1
    }

    private fun crc8(data: ByteArray): Int {
        var crc = 0
        for (bb in data) {
            crc = crc xor (bb.toInt() and 0xff)
            repeat(8) {
                crc = if (crc and 0x80 != 0) ((crc shl 1) xor 0x07) and 0xff else (crc shl 1) and 0xff
            }
        }
        return crc and 0xff
    }

    // seq is the device's 24-bit anti-replay counter: bytes [2],[3],[4].
    // byte[1]=0x24 is a fixed frame marker. Command is dropped unless seq >
    // the bulb's current stored counter (read it with tools/advert_dump.py).
    private fun frame(seq: Int, body: ByteArray): ByteArray {
        val pt = ByteArray(16)
        pt[0] = 0x07                          // DOWNLOAD_DP
        pt[1] = 0x24                          // frame marker
        pt[2] = ((seq ushr 16) and 0xff).toByte()
        pt[3] = ((seq ushr 8) and 0xff).toByte()
        pt[4] = (seq and 0xff).toByte()
        for (i in body.indices) pt[5 + i] = body[i]
        pt[14] = crc8(pt.copyOfRange(1, 14)).toByte()
        return xxteaEnc(pt)
    }

    fun onOff(seq: Int, on: Boolean): ByteArray =
        frame(seq, byteArrayOf(0x01, 0x01, 0x01, if (on) 1 else 0))

    fun brightness(seq: Int, level: Int): ByteArray {
        val v = byteArrayOf(
            (level ushr 24).toByte(), (level ushr 16).toByte(),
            (level ushr 8).toByte(), level.toByte()
        )
        return frame(seq, byteArrayOf(0x03, 0x02, 0x04) + v) // DP3 value 4B
    }

    /** UUID whose on-air bytes equal [ct]. Android serialises 128-bit UUIDs
     *  reversed, so by default we pre-reverse. */
    fun uuidForOnAir(ct: ByteArray, reverse: Boolean): UUID {
        val s = if (reverse) ct.reversedArray() else ct.copyOf()
        var msb = 0L; var lsb = 0L
        for (i in 0 until 8) msb = (msb shl 8) or (s[i].toLong() and 0xff)
        for (i in 8 until 16) lsb = (lsb shl 8) or (s[i].toLong() and 0xff)
        return UUID(msb, lsb)
    }

    fun hex(b: ByteArray): String = b.joinToString("") { "%02x".format(it) }
}

class MainActivity : ComponentActivity() {

    private val permLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // adb-driven orchestration: am start ... --ei seq N --ez on 0 --ei count 60 --ez auto 1
        run {
            val p = getSharedPreferences("forge", Context.MODE_PRIVATE)
            val ed = p.edit()
            if (intent?.hasExtra("seq") == true) ed.putInt("seq", intent.getIntExtra("seq", 0x00a000))
            if (intent?.hasExtra("count") == true) ed.putInt("count", intent.getIntExtra("count", 400))
            if (intent?.hasExtra("dur") == true) ed.putInt("dur", intent.getIntExtra("dur", 4000))
            if (intent?.hasExtra("name") == true) ed.putString("advname", intent.getStringExtra("name"))
            if (intent?.hasExtra("noname") == true) ed.putBoolean("noname", intent.getBooleanExtra("noname", false))
            if (intent?.getBooleanExtra("auto", false) == true) {
                ed.putBoolean("autoflag", true).putBoolean("auton", intent.getBooleanExtra("on", false))
            }
            ed.apply()
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            permLauncher.launch(
                arrayOf(Manifest.permission.BLUETOOTH_ADVERTISE, Manifest.permission.BLUETOOTH_CONNECT,
                    Manifest.permission.BLUETOOTH_SCAN)
            )
        }
        enableEdgeToEdge()
        setContent {
            TuyaBeaconForgeTheme {
                Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
                    ForgeScreen(applicationContext)
                }
            }
        }
    }
}

@SuppressLint("MissingPermission")
@Composable
fun ForgeScreen(ctx: Context) {
    val mgr = remember { ctx.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager }
    val adapter: BluetoothAdapter? = remember { mgr.adapter }
    val advertiser: BluetoothLeAdvertiser? = remember { adapter?.bluetoothLeAdvertiser }
    val scanner: BluetoothLeScanner? = remember { adapter?.bluetoothLeScanner }
    // Live counter learned by sniffing the bulb's own command beacons (op 03/07
    // from the official app OR our own broadcasts). Lets us always forge exactly
    // one ahead of the bulb, so commands are never stale or too-far-ahead.
    var liveCounter by remember { mutableStateOf(-1) }
    // Plaintext anti-replay SN read from the bulb's own status beacon (mfr data,
    // company 0xD007: head | mac[6] | sn[4 BE] | enc[16]). Per the Tuya beacon SDK
    // this is the NVRAM sequence number. Experiment: does sn+1 seed DOWNLOAD_DP?
    var statusSn by remember { mutableStateOf(-1L) }
    val handler = remember { Handler(Looper.getMainLooper()) }
    val sweepHandler = remember { Handler(Looper.getMainLooper()) }
    val prefs = remember { ctx.getSharedPreferences("forge", Context.MODE_PRIVATE) }
    var sweepCount by remember { mutableStateOf(prefs.getInt("count", 400)) }
    // 24-bit anti-replay counter; persisted so it survives restarts and stays
    // ahead of the bulb's stored value. Seed it just above the bulb's live
    // counter (read one real command with tools/advert_dump.py).
    var seq by remember { mutableStateOf(prefs.getInt("seq", 0x00a000)) }
    var durationMs by remember { mutableStateOf(3000) }
    var extended by remember { mutableStateOf(false) }
    var includeName by remember { mutableStateOf(!prefs.getBoolean("noname", false)) }
    // Must equal the controller (phone) BT name recorded when the bulb was paired.
    // Sniff it from a real command advert (tools/advert_dump.py) and set it here.
    var advName by remember { mutableStateOf(prefs.getString("advname", "YOUR_BT_NAME") ?: "YOUR_BT_NAME") }
    var reverseOrder by remember { mutableStateOf(true) }
    var bright by remember { mutableStateOf(500f) }
    var rawHex by remember { mutableStateOf("") }
    var log by remember { mutableStateOf("Ready.\nadapter=${adapter != null} advertiser=${advertiser != null} " +
        "ext=${if (Build.VERSION.SDK_INT >= 26) adapter?.isLeExtendedAdvertisingSupported else false}") }

    fun logln(s: String) { android.util.Log.i("FORGE", s); log = (s + "\n" + log).take(2000) }

    // self-test at startup: forge a couple of vectors and log them. With your real
    // localKey set, these must match the on-air bytes of equivalent app commands.
    LaunchedEffect(Unit) {
        android.util.Log.i("FORGE", "SELFTEST on=" + Forge.hex(Forge.onOff(0x002720, true)) +
            " off=" + Forge.hex(Forge.onOff(0x002720, false)))
    }

    // keep references so we can stop before re-broadcasting
    val legacyCb = remember { mutableStateOf<AdvertiseCallback?>(null) }
    val setCb = remember { mutableStateOf<AdvertisingSetCallback?>(null) }

    fun stopAll() {
        legacyCb.value?.let { runCatching { advertiser?.stopAdvertising(it) } }; legacyCb.value = null
        setCb.value?.let { runCatching { advertiser?.stopAdvertisingSet(it) } }; setCb.value = null
    }

    fun broadcast(ct: ByteArray, label: String) {
        if (advertiser == null) { logln("ERROR no advertiser (BT off / unsupported)"); return }
        stopAll()
        if (includeName && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            runCatching { adapter?.name = advName }
        }
        val uuid = Forge.uuidForOnAir(ct, reverseOrder)
        val data = AdvertiseData.Builder()
            .setIncludeDeviceName(includeName)
            .setIncludeTxPowerLevel(false)
            .addServiceUuid(ParcelUuid(uuid))
            .build()
        logln("TX $label  onair=${Forge.hex(ct)}  ext=$extended")
        val useExt = extended && Build.VERSION.SDK_INT >= 26 &&
            (adapter?.isLeExtendedAdvertisingSupported == true)
        if (useExt) {
            val params = AdvertisingSetParameters.Builder()
                .setLegacyMode(false)
                .setConnectable(false)
                .setScannable(false)
                .setInterval(AdvertisingSetParameters.INTERVAL_LOW)
                .setTxPowerLevel(AdvertisingSetParameters.TX_POWER_HIGH)
                .setPrimaryPhy(BluetoothDevice.PHY_LE_1M)
                .setSecondaryPhy(BluetoothDevice.PHY_LE_1M)
                .build()
            val cb = object : AdvertisingSetCallback() {
                override fun onAdvertisingSetStarted(s: AdvertisingSet?, txPower: Int, status: Int) {
                    logln(if (status == ADVERTISE_SUCCESS) "  ext started" else "  ext FAIL status=$status")
                }
            }
            setCb.value = cb
            runCatching { advertiser.startAdvertisingSet(params, data, null, null, null, cb) }
                .onFailure { logln("  ext exc=${it.message}") }
        } else {
            val settings = AdvertiseSettings.Builder()
                .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
                .setConnectable(true)   // app uses ADV_IND (advType=0) + auto Flags
                .setTimeout(durationMs.coerceIn(0, 180000))
                .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_HIGH)
                .build()
            val cb = object : AdvertiseCallback() {
                override fun onStartSuccess(s: AdvertiseSettings?) { logln("  legacy started ${durationMs}ms") }
                override fun onStartFailure(errorCode: Int) { logln("  legacy FAIL err=$errorCode (1=DATA_TOO_LARGE,4=TOO_MANY)") }
            }
            legacyCb.value = cb
            runCatching { advertiser.startAdvertising(settings, data, cb) }
                .onFailure { logln("  legacy exc=${it.message}") }
        }
        handler.removeCallbacksAndMessages(null)
        handler.postDelayed({ stopAll(); logln("  stopped") }, durationMs.toLong())
    }

    fun bumpSeq() { seq = (seq + 1) and 0xffffff; prefs.edit().putInt("seq", seq).apply() }
    // Forge one past whichever is higher: our own counter or the live-sniffed bulb
    // counter. Keeps us valid even if the official app advanced the bulb.
    fun send(on: Boolean) {
        val s = maxOf(seq, liveCounter + 1) and 0xffffff
        seq = s
        val ct = Forge.onOff(s, on); broadcast(ct, "DP1 ${if (on) "ON" else "OFF"} seq=0x%06x (live=0x%06x)".format(s, liveCounter)); bumpSeq()
    }

    // Passive scanner: track the bulb's anti-replay counter on-air, two ways:
    //  (a) command beacons (op 03/07) -> liveCounter  [only seen while commands fly]
    //  (b) the bulb's own status beacon (mfr data 0xD007) -> statusSn  [always on]
    DisposableEffect(scanner) {
        val cb = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult?) {
                val rec = result?.scanRecord ?: return
                // (a) command counter from a 128-bit service UUID = ciphertext
                rec.serviceUuids?.forEach { pu ->
                    val u = pu.uuid
                    val b = ByteArray(16)
                    var msb = u.mostSignificantBits; var lsb = u.leastSignificantBits
                    for (i in 7 downTo 0) { b[i] = (msb and 0xff).toByte(); msb = msb ushr 8 }
                    for (i in 15 downTo 8) { b[i] = (lsb and 0xff).toByte(); lsb = lsb ushr 8 }
                    val c = Forge.counterOf(b.reversedArray())
                    if (c in (liveCounter + 1)..0xffffff) liveCounter = c
                }
                // (b) plaintext SN from the Tuya status beacon: head|mac[6]|sn[4]|enc[16]
                rec.getManufacturerSpecificData(0xD007)?.let { md ->
                    if (md.size >= 11) {
                        var sn = 0L
                        for (i in 7..10) sn = (sn shl 8) or (md[i].toLong() and 0xff)
                        statusSn = sn
                    }
                }
            }
        }
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        runCatching { scanner?.startScan(null, settings, cb) }
            .onFailure { logln("scan start failed: ${it.message}") }
        onDispose { runCatching { scanner?.stopScan(cb) } }
    }

    // Sweep a contiguous range of counters quickly, so we cross the light's
    // current (unknown, moving) anti-replay value wherever it sits.
    val sweepCb = remember { mutableStateOf<AdvertiseCallback?>(null) }
    fun sweep(on: Boolean) {
        if (advertiser == null) { logln("no advertiser"); return }
        stopAll()
        handler.removeCallbacksAndMessages(null)
        sweepHandler.removeCallbacksAndMessages(null)
        if (includeName && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) runCatching { adapter?.name = advName }
        val settings = AdvertiseSettings.Builder()
            .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
            .setConnectable(true).setTimeout(0)
            .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_HIGH).build()
        val start = seq; val count = sweepCount; val stepMs = 120L
        logln("SWEEP ${if (on) "ON" else "OFF"} 0x%06x..0x%06x".format(start, (start + count - 1) and 0xffffff))
        var i = 0
        fun stop() { sweepCb.value?.let { runCatching { advertiser.stopAdvertising(it) } }; sweepCb.value = null }
        fun step() {
            if (i >= count) {
                stop(); seq = (start + count) and 0xffffff; prefs.edit().putInt("seq", seq).apply()
                logln("  sweep done; seq=0x%06x".format(seq)); return
            }
            stop()
            val s = (start + i) and 0xffffff
            val uuid = Forge.uuidForOnAir(Forge.onOff(s, on), reverseOrder)
            val data = AdvertiseData.Builder().setIncludeDeviceName(includeName)
                .setIncludeTxPowerLevel(false).addServiceUuid(ParcelUuid(uuid)).build()
            val cb = object : AdvertiseCallback() {}
            sweepCb.value = cb
            runCatching { advertiser.startAdvertising(settings, data, cb) }
            i++
            sweepHandler.postDelayed({ step() }, stepMs)
        }
        step()
    }

    // adb-driven auto-sweep: triggered by intent extras parsed in onCreate.
    LaunchedEffect(Unit) {
        if (prefs.getBoolean("autoflag", false)) {
            prefs.edit().putBoolean("autoflag", false).apply()
            seq = prefs.getInt("seq", 0x00a000)
            sweepCount = prefs.getInt("count", 400)
            val on = prefs.getBoolean("auton", false)
            val noName = prefs.getBoolean("noname", false)
            advName = prefs.getString("advname", "YOUR_BT_NAME") ?: "YOUR_BT_NAME"
            includeName = !noName
            if (!noName && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) runCatching { adapter?.name = advName }
            logln("auto name: ${if (noName) "<none>" else advName}")
            durationMs = prefs.getInt("dur", 4000)
            kotlinx.coroutines.delay(2000)           // let the adapter name propagate
            if (sweepCount <= 1) {
                logln("AUTO HOLD ${if (on) "ON" else "OFF"} seq=0x%06x dur=${durationMs}ms name=on".format(seq))
                broadcast(Forge.onOff(seq, on), "HOLD ${if (on) "ON" else "OFF"} seq=0x%06x".format(seq))
            } else {
                logln("AUTO sweep ${if (on) "ON" else "OFF"} from seq=0x%06x count=$sweepCount name=on".format(seq))
                sweep(on)
            }
        }
    }

    Column(
        Modifier.fillMaxSize().padding(16.dp).verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        Text("Tuya Beacon Forge", style = MaterialTheme.typography.headlineSmall)
        Text("Set localKey in Forge.KEY and the controller name below", style = MaterialTheme.typography.bodySmall)

        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Button(onClick = { send(true) }, modifier = Modifier.weight(1f)) { Text("TURN ON") }
            Button(onClick = { send(false) }, modifier = Modifier.weight(1f)) { Text("TURN OFF") }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Button(onClick = { sweep(true) }, modifier = Modifier.weight(1f)) { Text("SWEEP ON") }
            Button(onClick = { sweep(false) }, modifier = Modifier.weight(1f)) { Text("SWEEP OFF") }
        }

        Text("Brightness: ${bright.toInt()} (10–1000)")
        Slider(value = bright, onValueChange = { bright = it }, valueRange = 10f..1000f)
        Button(onClick = {
            broadcast(Forge.brightness(seq, bright.toInt()), "DP3 bright=${bright.toInt()} seq=0x%06x".format(seq))
            bumpSeq()
        }) { Text("SET BRIGHTNESS") }

        OutlinedTextField(
            value = rawHex, onValueChange = { rawHex = it.filter { c -> c.isLetterOrDigit() } },
            label = { Text("Raw 32-hex payload (replay/manual)") },
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Ascii),
            modifier = Modifier.fillMaxWidth()
        )
        Button(onClick = {
            val h = rawHex.trim()
            if (h.length == 32) {
                val ct = ByteArray(16) { ((Character.digit(h[it * 2], 16) shl 4) or Character.digit(h[it * 2 + 1], 16)).toByte() }
                broadcast(ct, "RAW")
            } else logln("raw must be 32 hex chars (got ${h.length})")
        }) { Text("BROADCAST RAW") }

        HorizontalDivider()
        Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
            Text("SEQ=0x%06x  bulb≈0x%06x".format(seq, liveCounter), modifier = Modifier.weight(1f), fontFamily = FontFamily.Monospace)
            OutlinedButton(onClick = { bumpSeq() }) { Text("SEQ+") }
            Spacer(Modifier.width(6.dp))
            OutlinedButton(onClick = { seq = 0x002720; prefs.edit().putInt("seq", seq).apply() }) { Text("reset") }
        }
        // EXPERIMENT: the bulb's status beacon carries a plaintext SN (Tuya beacon
        // SDK). If it shares the DOWNLOAD_DP counter space, seeding from status+1
        // and sweeping a little should land -> no command-sniff/dance ever needed.
        Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
            Text("status sn=" + (if (statusSn < 0) "—" else "0x%08x".format(statusSn)),
                modifier = Modifier.weight(1f), fontFamily = FontFamily.Monospace)
            Button(enabled = statusSn >= 0, onClick = {
                seq = ((statusSn + 1) and 0xffffffL).toInt()
                prefs.edit().putInt("seq", seq).apply()
                logln("SEED from status sn=0x%08x -> seq=0x%06x; sweeping ON to cross window".format(statusSn, seq))
                sweep(true)
            }) { Text("SEED FROM STATUS") }
        }
        Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
            Text("BLE5 extended", Modifier.weight(1f)); Switch(checked = extended, onCheckedChange = { extended = it })
        }
        Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
            Text("Include controller name", Modifier.weight(1f)); Switch(checked = includeName, onCheckedChange = { includeName = it })
        }
        Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
            Text("Reverse UUID byte order", Modifier.weight(1f)); Switch(checked = reverseOrder, onCheckedChange = { reverseOrder = it })
        }
        Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
            Text("Duration ${durationMs}ms", Modifier.weight(1f))
            OutlinedButton(onClick = { durationMs = (durationMs + 1000).coerceAtMost(15000) }) { Text("+1s") }
            Spacer(Modifier.width(6.dp))
            OutlinedButton(onClick = { durationMs = (durationMs - 1000).coerceAtLeast(1000) }) { Text("-1s") }
        }
        OutlinedButton(onClick = { stopAll(); logln("manual stop") }) { Text("STOP") }

        HorizontalDivider()
        Text("Log", style = MaterialTheme.typography.labelLarge)
        Text(log, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall)
    }
}
