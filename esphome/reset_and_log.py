"""Force the ESP32 to boot from flash (NOT download mode) and capture serial logs.

Some CH9102 / CH340 auto-program boards get trapped in the bootloader after an
`esphome run`/`upload` because the serial monitor asserts DTR (->GPIO0 low). This
helper drives the lines for a NORMAL boot -- DTR de-asserted (GPIO0 HIGH) the
whole time, pulse RTS (EN low->high) -- then streams the log.

    python reset_and_log.py COM7 30
"""
import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM7"
secs = int(sys.argv[2]) if len(sys.argv) > 2 else 15

s = serial.Serial(port, 115200, timeout=0.2)
s.dtr = False   # GPIO0 = HIGH (run mode, not download)
s.rts = True    # EN = LOW  -> chip held in reset
time.sleep(0.15)
s.dtr = False   # keep GPIO0 HIGH
s.rts = False   # EN = HIGH -> release reset, boot from flash
print("[reset_and_log] released reset, capturing %ds..." % secs, flush=True)

t0 = time.time()
while time.time() - t0 < secs:
    d = s.read(4096)
    if d:
        try:
            sys.stdout.write(d.decode("utf-8", "replace"))
        except Exception:
            sys.stdout.write(repr(d))
        sys.stdout.flush()
s.close()
print("\n[reset_and_log] done.", flush=True)
