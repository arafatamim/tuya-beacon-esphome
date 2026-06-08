# tuya-beacon

An ESPHome custom component for cheap Tuya Beacon RGBCW bulbs — the sub-$5 AliExpress ones
that use a proprietary connectionless BLE protocol no existing Home Assistant integration supports.

Flash it onto a classic ESP32, point it at your bulbs in YAML, and they show up in Home Assistant
as native light entities with full on/off, brightness, colour temperature, and RGB — no Tuya app or gateway.

---

## Is this your bulb?

These bulbs advertise as **connectionless** — you can't GATT-connect to them from anything. Scan
with nRF Connect or a BLE sniffer and you'll see manufacturer data with company ID `0xD007` and
a MAC often starting `DC:23:4x:xx:xx:xx` (Phyplus PHY6230 chip).

If your bulb *is* connectable, it's a different protocol:
- Connectable Tuya BLE → [`PlusPlus-ua/ha_tuya_ble`](https://github.com/PlusPlus-ua/ha_tuya_ble)
- AwoX / SIG Mesh → [`11z4t/tuya-ble-mesh`](https://github.com/11z4t/tuya-ble-mesh)

---

## Quick start

### 1. Extract your credentials (one-time)

You need three things per bulb:

| Item | How to get it |
|---|---|
| **`localKey`** (16 ASCII chars) | See below — two methods depending on what you have available. Rotates only if you re-pair. |
| **Controller name** | The Bluetooth name of the phone that paired it. Check your phone's BT settings, or sniff it with `tools/advert_dump.py`. |
| **Counter seed** | `python tools/status_seed.py 15 <bulb-MAC>` — reads the live counter straight from the bulb's status beacon. No app needed. |

#### Extracting the localKey

**Method 1 — Android emulator + shared_prefs (no root, no Frida)**

Smart Life **≤ 3.6.1** stores device credentials in a plain XML SharedPreferences
file. Newer versions switched to binary MMKV storage, so use an older APK.

1. Create a **Google APIs** Android emulator in Android Studio — *not* "Google
   Play Store". The APIs image allows `adb root`; the Play Store image does not.
2. Install Smart Life 3.6.1 (available on APKMirror) and log in:
   ```bash
   adb install SmartLife-3.6.1.apk
   ```
3. Pair your bulbs under this account, then pull the shared preferences:
   ```bash
   adb root
   adb pull /data/data/com.tuya.smartlife/shared_prefs/
   ```
4. Open the file named `preferences_global_key<your_account_id>.xml`. It
   contains several `<string>` nodes whose values are HTML-entity-encoded JSON.
   The relevant one has this structure once decoded:
   ```json
   {
     "deviceRespBeen": [
       { "name": "Bedroom Light", "devId": "...", "localKey": "YOURLOCALKEY1234" },
       ...
     ]
   }
   ```
   [TuyaKeyExtractor](https://github.com/MarkWattTech/TuyaKeyExtractor) is a
   ready-made .NET tool that parses this file and prints a clean table of keys.

**Method 2 — Frida (any app version)**

If you can't use an older APK, see [`frida/`](frida/) for a runtime hook that
intercepts keys from the running app. Requires Frida on a rooted device or
emulator.

### 2. Add the component to your ESPHome config

Copy this repo's `esphome/` directory alongside your YAML, then:

```yaml
external_components:
  - source:
      type: local
      path: esphome/components

esp32_ble:
esp32_ble_tracker:
  scan_parameters:
    active: false
    interval: 600ms
    window: 60ms

tuya_beacon:
  - id: bedroom_bulb
    local_key: !secret bulb_local_key
    controller_name: !secret bulb_controller_name
    mac_address: !secret bulb_mac_address
    initial_seq: !secret bulb_initial_seq   # from status_seed.py, one-time

light:
  - platform: tuya_beacon
    tuya_beacon_id: bedroom_bulb
    name: "Bedroom"
```

Add more `tuya_beacon` + `light` blocks for additional bulbs — the component is `MULTI_CONF`.

See [`esphome/home-gateway.example.yaml`](esphome/home-gateway.example.yaml) and
[`esphome/secrets.example.yaml`](esphome/secrets.example.yaml) for a complete working example.

### 3. Flash and adopt in Home Assistant

```bash
esphome compile your-config.yaml
esphome upload  your-config.yaml --device COM_PORT   # or --device IP for OTA
```

The ESP32 appears in **Settings → Devices & Services → ESPHome** in Home Assistant. Adopt it and
the bulbs show up as light entities.

---

## How the counter works

The bulb uses a 24-bit anti-replay counter — it only accepts a command whose sequence number is
just above the stored value. The component handles this entirely on its own:

- The counter is **persisted to NVS** after every command, so reboots are instant.
- On boot it **listens to the bulb's own status beacon** and cross-checks for drift (e.g. if the
  app was used in the meantime).
- Once the app is retired, the counter only moves when the ESP32 moves it, so it never drifts.

The `initial_seq` you set in `secrets.yaml` is only used on a cold start (empty NVS). After the
first command it's replaced by the real persisted value. You only need to get it roughly right —
`tools/status_seed.py` gives you the exact current value.

---

## Repo layout

```
esphome/
  components/
    tuya_beacon/           The ESPHome external component
      __init__.py          Schema: localKey, controller_name, mac_address, initial_seq
      tuya_beacon.h/.cpp   XXTEA cipher, CRC-8, NVS counter, BLE GAP advertising,
                           status beacon parsing (ESPBTDeviceListener)
      light/
        __init__.py        Light platform: RGB + colour temperature traits
        tuya_beacon_light.h/.cpp  write_state() → DP commands
  home-gateway.example.yaml
  secrets.example.yaml

tools/                     Desktop utilities (Windows / WinRT)
  forge.py                 XXTEA + CRC-8 + frame builder — importable library
  advert_dump.py           Sniff and decrypt live command adverts
  status_seed.py           Read the bulb's live command counter from its status beacon

frida/
  dump_localkey.js         Extract localKey from the Smart Life app at runtime

android/
  BeaconForge/             Android app for one-shot command broadcasting (testing / no hardware)

PROTOCOL.md                Full byte-level protocol specification
```

---

## Protocol

The short version: to control a bulb you broadcast a single connectable BLE advertisement
(`ADV_IND`) carrying an XXTEA-encrypted 16-byte payload in the 128-bit Service UUID field.
The local name in the advert must match the name used at pairing. That's it.

For the full byte layout, all datapoints (on/off, brightness, colour temp, RGB HSV), the cipher
details, and the counter mechanics, see [PROTOCOL.md](PROTOCOL.md).

---

## Legal

Interoperability research for hardware you own. Keep your `localKey` private — don't commit it.
Don't use this on devices you don't own.

MIT licensed — see [LICENSE](LICENSE).

---

## Credits

- [elektroda: "Reverse Engineering Tuya Beacon Protocol"](https://www.elektroda.com/rtvforum/topic4092517.html) (holchansgomes) — first public characterisation of the chip and packet format.
- [tuya/tuya-iotos-beacon-sdk-ak80x](https://github.com/tuya/tuya-iotos-beacon-sdk-ak80x) — official Tuya SDK confirming the `0xD007` wire format and XXTEA usage.
- [pvvx/PHY62x2](https://github.com/pvvx/PHY62x2) — PHY6230 chip documentation.
- The Tuya BLE / SIG Mesh projects above, for clearly marking out which protocol is which.
