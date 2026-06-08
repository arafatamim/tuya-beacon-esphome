"""Read a Tuya Beacon bulb's anti-replay COMMAND counter straight off its
status beacon -- no official app, no command sniffing.

A beacon device broadcasts manufacturer data under company ID 0xD007:

    company 0xD007 | head(0x04) | mac[6] | sn[4 BE] | enc[16]

IMPORTANT -- two different counters live here:
  * the CLEARTEXT `sn` (bytes 7:11) is a fast STATUS sequence number (it ticks on
    every status broadcast); it is NOT the DOWNLOAD_DP command counter.
  * the COMMAND counter (what your forger must seed from) is inside the ENCRYPTED
    payload: decrypt enc[16] with your localKey and read pt[4:8] (big-endian),
    on reports flagged by the `ff ff ff` marker at pt[8:11].

So you DO need the localKey here. This was verified on hardware: seeding a forger
from pt[4:8] (+ a tiny forward sweep) lands; seeding from the cleartext `sn` does
not. The ESPHome firmware in ../esphome uses exactly this to self-sync.

    export TUYA_LOCALKEY=YOUR16CHARKEY1234
    python tools/status_seed.py 15                       # sniff 15s, any bulb
    python tools/status_seed.py 15 DC:23:4E:AA:BB:CC     # only this MAC

Requires: pip install winrt
"""
import asyncio
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import forge  # noqa: E402

import winrt.windows.devices.bluetooth.advertisement as adv
import winrt.windows.storage.streams as streams

TUYA_COMPANY_ID = 0xD007
RUN_S = int(sys.argv[1]) if len(sys.argv) > 1 else 15
WANT_MAC = sys.argv[2].upper().replace(":", "") if len(sys.argv) > 2 else None

latest = {}  # mac -> command_counter


def buf(ibuf):
    if ibuf is None:
        return b""
    r = streams.DataReader.from_buffer(ibuf)
    return bytes(r.read_byte() for _ in range(ibuf.length))


def on_recv(w, args):
    a = args.advertisement
    for md in a.manufacturer_data:
        if md.company_id != TUYA_COMPANY_ID:
            continue
        d = buf(md.data)
        # head(0x04) | mac[6] | sn[4] | enc[16]  == 27 bytes
        if len(d) < 27 or d[0] != 0x04:
            continue
        mac_str = ":".join("%02X" % b for b in d[1:7])
        mac_key = "".join("%02X" % b for b in d[1:7])
        if WANT_MAC and mac_key != WANT_MAC:
            continue
        status_sn = int.from_bytes(d[7:11], "big")  # cleartext: NOT the cmd counter
        pt = forge.xxtea_dec(d[11:27])
        if not (pt[8] == 0xFF and pt[9] == 0xFF and pt[10] == 0xFF):
            continue  # not a counter-bearing status report
        cmd_counter = int.from_bytes(pt[4:8], "big")
        prev = latest.get(mac_str)
        latest[mac_str] = cmd_counter
        if prev is None or prev != cmd_counter:
            print(f"  {mac_str}  status_sn=0x{status_sn:08x}  "
                  f"command_counter=0x{cmd_counter:06x} ({cmd_counter})"
                  f"{'   <-- changed' if prev is not None else ''}")


async def main():
    if forge.KEY == b"YOURLOCALKEY1234":
        print("! Set TUYA_LOCALKEY=<your 16-char localKey> -- decryption needs it.\n")
    w = adv.BluetoothLEAdvertisementWatcher()
    w.scanning_mode = adv.BluetoothLEScanningMode.ACTIVE
    w.add_received(on_recv)
    print(f"Sniffing Tuya status beacons (company 0x{TUYA_COMPANY_ID:04x}) for {RUN_S}s"
          + (f", MAC {sys.argv[2]}" if WANT_MAC else "") + " ...")
    print("Toggle the bulb (any way) during the window to watch the counter move.\n")
    w.start()
    await asyncio.sleep(RUN_S)
    w.stop()

    if not latest:
        print("\nNo counter-bearing 0xD007 status beacons decoded. Is the bulb powered,"
              " in range, and is TUYA_LOCALKEY correct?")
        return
    print("\n=== latest command counters ===")
    for mac_str, c in latest.items():
        seed = (c + 1) & 0xFFFFFF
        print(f"{mac_str}  command_counter=0x{c:06x}  ->  seed your forger at counter+1 = "
              f"{seed} (0x{seed:06x}), with a small forward sweep to cover the window")
        print("  Android BeaconForge:")
        print(f"    adb shell am start -n com.example.tuyabeaconforge/.MainActivity \\")
        print(f"      --ei seq {seed} --ez on 1 --ei count 12 --ez auto 1 --es name YOUR_BT_NAME")


asyncio.run(main())
