# Tuya Beacon — Protocol Specification

Reverse-engineered against a Super Star Bluetooth Smart LED 9W RGBCW bulb (Phyplus PHY6230, "Tuya Beacon" firmware).
All claims verified by forging commands that the bulb physically obeys with no app.

---

## 1. Overview

The bulb is **receive-only** for control. It never accepts a GATT connection — it only scans BLE
advertisements. Control is one-way: the controller broadcasts an encrypted command advert; the
bulb decrypts, validates the anti-replay counter, and acts. There is no acknowledgement.

---

## 2. Status beacon (bulb → world)

The bulb continuously broadcasts manufacturer data under company ID `0xD007`:

```
04 | MAC[6] | sn[4 BE] | enc[16]
```

`sn` is a fast status sequence number (increments on every broadcast). The **command counter**
lives inside the encrypted `enc` field — not in the cleartext `sn`. To read it, decrypt `enc`
with the localKey using XXTEA and read `pt[4:8]` (big-endian) on reports where `pt[8:11]` is
`ff ff ff`. See `tools/status_seed.py`.

---

## 3. Command advert (controller → bulb)

A **connectable legacy advertisement** (`ADV_IND`, ≤31 bytes). Three AD structures, in order:

```
02 01 06              Flags (LE General Discoverable + BR/EDR not supported)
<len> 09 <name>       Complete Local Name — controller name from pairing (≤8 chars recommended)
11 07 <ct[16]>        Complete 128-bit Service UUID — the 16-byte XXTEA ciphertext, forward/on-air order
```

Two things that must be correct:

1. **The advert must be connectable (`ADV_IND`).** A non-connectable advert is silently ignored,
   even with a perfect payload.
2. **The local name must match** the Bluetooth name of the controller that originally paired the
   bulb (e.g. your phone's BT name). A different name is rejected.

**On Android:** `ParcelUuid` serializes UUIDs reversed relative to on-air order. Pass the
byte-reversed ciphertext as the service UUID so the correct bytes appear on air.

**On ESP32:** use `esp_ble_gap_config_adv_data_raw()` + `ADV_TYPE_IND` with the raw 31-byte
buffer. The ciphertext goes in directly in forward order.

---

## 4. Command plaintext (16 bytes)

```
[0]     opcode     0x07 = DOWNLOAD_DP
[1]     marker     0x24 (fixed)
[2:5]   seq        24-bit anti-replay counter, big-endian
[5:14]  DP data    one or more compact datapoints (see below)
[14]    CRC-8      poly 0x07, init 0x00, no reflection, over bytes [1:14]
[15]    pad        0x00
```

### Datapoints (compact Tuya format)

Each datapoint: `dp_id | dp_type | dp_len[2 BE] | value[dp_len]`

| DP | ID | Type | Len | Value | Notes |
|---|---|---|---|---|---|
| On/off | `01` | `01` bool | 1 | `01`=on, `00`=off | `01 01 01 VV` |
| Mode | `02` | `04` enum | 1 | `00`=white, `01`=colour | Send before brightness/colour |
| Brightness | `03` | `02` value | 4 | 10–1000, big-endian | `03 02 04 00 00 HH LL` |
| Color temp | `04` | `02` value | 4 | 0 (warm) – 1000 (cool), big-endian | `04 02 04 00 00 HH LL` |
| Colour (HSV) | `0b` | `00` raw | 4 | `HH HL` hue 0–360 + `SS` sat 0–100 + `VV` val 0–100 | `0b 00 04 HH HL SS VV` |

---

## 5. Cipher: XXTEA

- Standard XXTEA, **big-endian** word packing (4 × `uint32`), `DELTA = 0x9E3779B9`, 19 rounds.
- **Key = the device `localKey` as 16 raw ASCII bytes.** Do not hex-decode it. A key printed as
  `A1B2C3D4E5F60718` is the literal bytes `b"A1B2C3D4E5F60718"`.
- The localKey **rotates on every re-pair** — re-extract with Frida if you re-pair.

Reference implementations: `tools/forge.py` (Python), `android/BeaconForge` (Kotlin).

---

## 6. Anti-replay counter

The bulb accepts a command only if its `seq` is **just above** the stored value. The forward
window is narrow — a few counts are accepted, large jumps (hundreds or more) are rejected.

**Reading the current counter** (required once at setup):

```bash
export TUYA_LOCALKEY=YOURLOCALKEY1234
python tools/status_seed.py 15 DC:23:4E:xx:xx:xx
```

This decrypts the bulb's status beacon and prints the live command counter. No app needed.

**Staying in sync** after that is automatic: as sole controller, increment `seq` by 1 for each
command and persist it (the ESPHome component does this in NVS). If the app is used and moves
the counter ahead, the status beacon self-seeding catches the drift automatically.

---

## 7. Transmitter requirements

A PC generally **cannot** transmit these commands — Windows/WinRT and most BlueZ setups cannot
build a connectable advert with both a custom local name and a 128-bit service UUID.

- **Android:** `AdvertiseData` + `setIncludeDeviceName(true)` + `addServiceUuid()` +
  `AdvertiseSettings.setConnectable(true)`. Set the device name first via `BluetoothAdapter.name`.
- **ESP32:** `esp_ble_gap_config_adv_data_raw()` + `ADV_TYPE_IND`. Classic ESP32 is fine — this
  is a legacy BLE 4.2 advert, not extended. The [ESPHome component](esphome/) is the recommended
  path for permanent, multi-bulb, Home Assistant-integrated use.
