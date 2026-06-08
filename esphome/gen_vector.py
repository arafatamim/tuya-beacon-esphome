"""Generate the C++ self-test vectors for tuya_beacon.h's tb_self_test().

The on-device self-test asserts that the firmware's XXTEA+CRC+framing produce the
exact ciphertext for your localKey -- catching any port/build mistake before a
single packet hits the air. The default vectors in tuya_beacon.h are for the
PLACEHOLDER key; regenerate for YOUR key and paste the two lines it prints.

    TUYA_LOCALKEY=YOUR16CHARKEY1234 python esphome/gen_vector.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import forge  # noqa: E402

if forge.KEY == b"YOURLOCALKEY1234":
    print("! Using placeholder key. Set TUYA_LOCALKEY=<your 16-char localKey>.\n")

print("Paste into tb_self_test() in tuya_beacon.h:\n")
for seq, on in ((0x002720, True), (0x002720, False)):
    _, ct = forge.onoff(seq, on)
    print('      {0x%06x, %s, "%s"},' % (seq, "true " if on else "false", ct.hex()))
