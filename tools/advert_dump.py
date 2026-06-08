"""Sniff live Tuya Beacon command adverts and dump them (Windows / WinRT).

Detects a command by trying to XXTEA-decrypt each 128-bit service UUID with your localKey; if the
plaintext looks like a DOWNLOAD_DP/poll frame (op 01/03/06/07, marker 0x24) it prints the advert
type, local name, every AD section, and the decoded 24-bit counter. Use it to:

  * learn the controller name your bulb expects (the Complete Local Name AD),
  * read the bulb's current anti-replay counter (so you forge just above it),
  * diff a real app command against your own broadcast.

    export TUYA_LOCALKEY=YOURLOCALKEY1234
    python tools/advert_dump.py 30        # sniff for 30 seconds

Requires: pip install winrt  (Windows 10/11 with a BLE adapter).
"""
import asyncio
import sys
import uuid as uuidmod

import winrt.windows.devices.bluetooth.advertisement as adv
import winrt.windows.storage.streams as streams

from forge import xxtea_dec

RUN_S = int(sys.argv[1]) if len(sys.argv) > 1 else 25
seen = {}

AD_NAMES = {
    0x01: "Flags", 0x02: "UUID16inc", 0x03: "UUID16", 0x06: "UUID128inc",
    0x07: "UUID128", 0x08: "NameShort", 0x09: "NameComplete",
    0x0A: "TxPower", 0xFF: "MfrData", 0x16: "SvcData16", 0x21: "SvcData128",
}


def buf(ibuf):
    if ibuf is None:
        return b""
    r = streams.DataReader.from_buffer(ibuf)
    return bytes(r.read_byte() for _ in range(ibuf.length))


def is_cmd(a):
    for g in a.service_uuids:
        try:
            u = uuidmod.UUID(str(g))
        except Exception:
            continue
        # WinRT reports on-air order; also try reversed in case of endian quirks.
        for cand in (u.bytes[::-1], u.bytes):
            pt = xxtea_dec(cand)
            if pt[0] in (0x01, 0x03, 0x06, 0x07) and pt[1] == 0x24:
                seq = (pt[2] << 16) | (pt[3] << 8) | pt[4]
                return (pt[0], seq, pt.hex())
    return None


def on_recv(w, args):
    a = args.advertisement
    info = is_cmd(a)
    if not info:
        return
    mac = ":".join("%02x" % b for b in args.bluetooth_address.to_bytes(6, "big"))
    secs = [(s.data_type, buf(s.data).hex()) for s in a.data_sections]
    sig = (mac[:8], tuple(secs))
    if sig in seen:
        return
    seen[sig] = True
    op, seq, pt = info
    print(f"\n--- CMD op={op:#x} seq=0x{seq:06x} from {mac} "
          f"advType={int(args.advertisement_type)} name={a.local_name!r} ---")
    print(f"    pt={pt}")
    for t, d in secs:
        print(f"    AD 0x{t:02x} {AD_NAMES.get(t, '?'):12} ({len(d) // 2:2}B) {d}")


async def main():
    w = adv.BluetoothLEAdvertisementWatcher()
    w.scanning_mode = adv.BluetoothLEScanningMode.ACTIVE
    try:
        w.allow_extended_advertisements = True
    except AttributeError:
        pass
    w.add_received(on_recv)
    print(f"Dumping command adverts (decrypt-detected) for {RUN_S}s...")
    w.start()
    await asyncio.sleep(RUN_S)
    w.stop()
    print(f"\n{len(seen)} distinct (source, layout) combos.")


asyncio.run(main())
