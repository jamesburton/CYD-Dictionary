# tools/boot_log.py — reset the device over COM5 and print ~8s of boot output.
import serial, sys, time
port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
s = serial.Serial(port, 115200, timeout=0.3)
s.setDTR(False); s.setRTS(True); time.sleep(0.2); s.setRTS(False)
end = time.time() + 8
while time.time() < end:
    line = s.readline()
    if line:
        print(line.decode(errors="replace").rstrip())
s.close()
