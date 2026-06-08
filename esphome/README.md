# ESPHome component

Full quick-start is in the [root README](../README.md). This file covers implementation details and gotchas for anyone reading the component source.

---

## Component layout

```
components/tuya_beacon/
  __init__.py               Schema: local_key, controller_name, mac_address, initial_seq
  tuya_beacon.h/.cpp        XXTEA cipher, CRC-8, NVS counter, BLE GAP advertising,
                            status beacon parsing (ESPBTDeviceListener::parse_device)
  light/
    __init__.py             Light platform: RGB + colour temperature traits
    tuya_beacon_light.h/.cpp  write_state() → DP commands
```

`MULTI_CONF = True` — add as many `tuya_beacon:` blocks as you have bulbs.

---

## How the counter stays in sync

The bulb continuously broadcasts a `0xD007` manufacturer-data beacon. The component registers as an `ESPBTDeviceListener` via `esp32_ble_tracker` and receives every advertisement via `parse_device()`.

On each beacon from the matched MAC, it XXTEA-decrypts the 16-byte `enc` field. When the decrypted `pt[8:11]` == `ff ff ff`, `pt[4:8]` (big-endian) is the current command counter. If that value is ahead of the stored counter, the component updates immediately.

This means: if the official app is used, the counter drift self-corrects on the very next status beacon — no manual re-seeding, no reflash.

The counter is persisted to NVS after every command (keyed by `bulb_mac & 0xFFFFFFFF ^ 0x54424332`), so reboots are instant with no re-sync needed.

The `initial_seq` in `secrets.yaml` is only used on a truly cold start (empty NVS). Use `tools/status_seed.py` to read the live value.

---

## How commands are sent

Each command is a 16-byte XXTEA-encrypted plaintext broadcast as a **connectable `ADV_IND`** (not non-connectable — the bulb firmware silently ignores `ADV_NONCONN_IND`).

The three AD structures in the advert:

```
02 01 06              Flags
<len> 09 <name>       Complete Local Name (controller_name from config)
11 07 <ct[16]>        128-bit Service UUID = XXTEA ciphertext
```

Each command sequence: `stop advertising → config_adv_data_raw → delay(15ms) → start advertising → hold (1200ms) → stop`. The hold is necessary — `config_adv_data_raw` does not update a live advert; a full stop/start is required per payload.

On cold start (no NVS), a bootstrap sweep tries `initial_seq` through `initial_seq + 0x40` at 250 ms/seq until the bulb responds.

---

## BLE coexistence

`esp32_ble_tracker` scanning and BLE advertising share the same radio. Continuous scanning at default duty starves the advertiser and the bulb misses commands. Throttle the scanner heavily:

```yaml
esp32_ble_tracker:
  scan_parameters:
    active: false
    interval: 600ms
    window: 60ms   # ~10% duty
```

---

## Flashing

Use `compile` + `upload`, not `run`. The serial monitor attached by `run` can hold DTR/GPIO0 low on some CH9102/CH340 boards, trapping the chip in the bootloader. After the first USB flash, prefer OTA.

```bash
esphome compile your-config.yaml
esphome upload  your-config.yaml --device COM7   # or IP for OTA
```

If the board boots stuck at "waiting for download", power-cycle it or use `reset_and_log.py`.

---

## Files

| File | Purpose |
|------|---------|
| `components/tuya_beacon/` | The ESPHome external component (source of truth) |
| `home-gateway.example.yaml` | Complete example config — copy and adapt |
| `secrets.example.yaml` | Template for `secrets.yaml` (gitignored) |
| `gen_vector.py` | Generate C++ XXTEA self-test vectors for a given key |
| `reset_and_log.py` | Force clean boot + capture serial log (CH9102 workaround) |
