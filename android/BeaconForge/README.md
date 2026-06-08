# BeaconForge (Android)

A minimal Jetpack Compose app that broadcasts the **connectable** Tuya Beacon command advert.
A phone is used (not a PC) because Android can put a 128-bit service UUID **and** a custom local
name into a connectable legacy advert; WinRT/most BlueZ setups can't.

## Set two things before building

In `app/src/main/java/com/example/tuyabeaconforge/MainActivity.kt`:

1. `Forge.KEY` — your bulb's 16-char **localKey** (see the README's "Extracting the localKey" section — adb backup is the easiest route).
2. The **controller name** (the UI's "Include controller name" field / `advname` pref) — the BT
   name of the phone that paired the bulb. Sniff it with `../../tools/advert_dump.py`.

Also seed `seq` just above the bulb's current counter (read one real command with `advert_dump.py`),
or use the **SWEEP** buttons to cross the counter automatically.

## Build & run

Open in Android Studio, or:

```bash
./gradlew installDebug
adb shell pm grant com.example.tuyabeaconforge android.permission.BLUETOOTH_ADVERTISE
adb shell pm grant com.example.tuyabeaconforge android.permission.BLUETOOTH_CONNECT
adb shell pm grant com.example.tuyabeaconforge android.permission.BLUETOOTH_SCAN
```

## UI

- **TURN ON / OFF** — forge + broadcast a DP1 command at `max(seq, liveCounter+1)`.
- **SWEEP ON / OFF** — rapidly walk a range of counters to cross the bulb's current value when you
  don't know it.
- **status sn / SEED FROM STATUS** — shows the plaintext SN read live from the bulb's status beacon
  (mfr data `0xD007`). The button seeds `seq = sn+1` and sweeps ON to test whether the status SN
  shares the command counter space (the no-sniff seeding experiment).
- **Brightness** — DP3 value (10–1000).
- **Raw 32-hex** — replay/broadcast an arbitrary 16-byte payload.
- It passively scans and shows `bulb≈` — the live counter learned from on-air commands, so you can
  always stay exactly one ahead.

The app also accepts intent extras for adb-driven automation, e.g.:

```bash
adb shell am start -n com.example.tuyabeaconforge/.MainActivity \
  --ei seq 41024 --ez on 1 --ei count 1 --ez auto 1 --es name YOUR_BT_NAME
```
