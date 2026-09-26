"""Capture COMx boot log for N seconds (bench bring-up helper, not tracked)."""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM14"
DURATION = float(sys.argv[2]) if len(sys.argv) > 2 else 45.0

ser = serial.Serial(PORT, 115200, timeout=0.2)
# Windows drivers re-assert DTR/RTS on open -> clear them NOW, after open,
# otherwise the RTS pulse below enters ROM download mode (DTR=IO0 low)
ser.dtr = False
ser.rts = False
time.sleep(0.1)
ser.reset_input_buffer()

# esptool classic hard reset: pulse EN low via RTS while IO0 (DTR) is high
ser.rts = True
time.sleep(0.1)
ser.rts = False

t0 = time.time()
while time.time() - t0 < DURATION:
    data = ser.read(4096)
    if data:
        sys.stdout.write(f"[{time.time() - t0:7.2f}] ")
        sys.stdout.write(data.decode("utf-8", errors="replace").replace("\r\n", "\n"))
        sys.stdout.flush()
ser.close()
print(f"\n--- capture end after {time.time() - t0:.1f}s ---")
