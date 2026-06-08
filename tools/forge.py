"""Tuya Beacon command forger: XXTEA (big-endian) + CRC-8 + DOWNLOAD_DP framing.

Build the 16-byte encrypted payload that goes into the 128-bit Service UUID of a connectable
BLE advertisement (see PROTOCOL.md). A PC generally can't *broadcast* it (WinRT/BlueZ can't set a
service-UUID + local-name connectable advert) -- use the Android app or an ESP32 for TX. This
module is for building/decrypting/verifying, and is imported by advert_dump.py.

Key = your device localKey as 16 raw ASCII bytes (NOT hex-decoded). It rotates on every re-pair.
Supply it via the TUYA_LOCALKEY env var or pass key= explicitly.

    export TUYA_LOCALKEY=YOURLOCALKEY1234
    python tools/forge.py
"""
import os
import struct

# 16 ASCII chars. Override with env TUYA_LOCALKEY or the key= argument on each function.
KEY = os.environ.get("TUYA_LOCALKEY", "YOURLOCALKEY1234").encode("ascii")
DELTA = 0x9E3779B9


def _u(b):
    return list(struct.unpack(">4I", b))


def _p(v):
    return struct.pack(">4I", *[w & 0xFFFFFFFF for w in v])


def xxtea_enc(block, key=None):
    k = _u((key or KEY)); v = _u(block); n = 4; s = 0; z = v[n - 1]
    for _ in range(6 + 52 // n):
        s = (s + DELTA) & 0xFFFFFFFF
        e = (s >> 2) & 3
        for p in range(n):
            y = v[(p + 1) % n]
            mx = (((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^ ((s ^ y) + (k[(p & 3) ^ e] ^ z))
            v[p] = (v[p] + mx) & 0xFFFFFFFF; z = v[p]
    return _p(v)


def xxtea_dec(block, key=None):
    k = _u((key or KEY)); v = _u(block); n = 4
    s = ((6 + 52 // n) * DELTA) & 0xFFFFFFFF; y = v[0]
    while s != 0:
        e = (s >> 2) & 3
        for p in range(n - 1, -1, -1):
            z = v[(p - 1) % n]
            mx = (((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^ ((s ^ y) + (k[(p & 3) ^ e] ^ z))
            v[p] = (v[p] - mx) & 0xFFFFFFFF; y = v[p]
        s = (s - DELTA) & 0xFFFFFFFF
    return _p(v)


def crc8(data):
    """CRC-8, poly 0x07, init 0x00, no reflection, no xor-out."""
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc & 0xFF


def frame(seq, body, key=None):
    """Build (plaintext, ciphertext) for a DOWNLOAD_DP command.

    seq  = 24-bit anti-replay counter (must be just above the bulb's current value).
    body = compact datapoint bytes: dp_id | dp_type | dp_len | value...
    """
    pt = bytearray(16)
    pt[0] = 0x07            # DOWNLOAD_DP
    pt[1] = 0x24            # frame marker
    pt[2] = (seq >> 16) & 0xFF
    pt[3] = (seq >> 8) & 0xFF
    pt[4] = seq & 0xFF
    for i, b in enumerate(body):
        pt[5 + i] = b
    pt[14] = crc8(bytes(pt[1:14]))
    return bytes(pt), xxtea_enc(bytes(pt), key)


def onoff(seq, on, key=None):
    """DP1 (bool) on/off."""
    return frame(seq, bytes([0x01, 0x01, 0x01, 1 if on else 0]), key)


def brightness(seq, level, key=None):
    """DP3 (value, 4B big-endian, 10..1000)."""
    v = struct.pack(">I", level & 0xFFFFFFFF)
    return frame(seq, bytes([0x03, 0x02, 0x04]) + v, key)


def counter_of(onair16, key=None):
    """If onair16 is a valid command (op 03/06/07, marker 0x24) return its 24-bit
    counter, else -1. Used to read a bulb's live counter from a sniffed advert."""
    if len(onair16) != 16:
        return -1
    pt = xxtea_dec(onair16, key)
    if pt[0] in (0x03, 0x06, 0x07) and pt[1] == 0x24:
        return (pt[2] << 16) | (pt[3] << 8) | pt[4]
    return -1


if __name__ == "__main__":
    if KEY == b"YOURLOCALKEY1234":
        print("! Using placeholder key. Set TUYA_LOCALKEY=<your 16-char localKey> for real output.\n")
    # self-test: encrypt/decrypt are inverses, CRC and framing are stable.
    pt, ct = onoff(0x002720, True)
    back = xxtea_dec(ct)
    print("plaintext :", pt.hex())
    print("ciphertext:", ct.hex())
    print("dec(ct)   :", back.hex(), "OK" if back == pt else "FAIL")
    print("counter_of:", hex(counter_of(ct)))
    for s in (0x002720, 0x009C70):
        for on in (True, False):
            _, c = onoff(s, on)
            print(f"seq={s:#08x} {'ON ' if on else 'OFF'} -> {c.hex()}")
