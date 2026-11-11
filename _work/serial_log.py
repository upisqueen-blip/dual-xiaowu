"""抓串口日志：用于观察 A 屏（SSD1619）初始化与刷新流程。"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
seconds = int(sys.argv[2]) if len(sys.argv) > 2 else 45

ser = serial.Serial(port, 115200, timeout=0.2)
# 打开串口会拉 DTR/RTS 触发复位，正好从启动日志开始看
ser.setDTR(False)
ser.setRTS(True)
time.sleep(0.1)
ser.setRTS(False)
time.sleep(0.1)

end = time.time() + seconds
buf = b""
while time.time() < end:
    try:
        chunk = ser.read(4096)
    except Exception as e:  # noqa: BLE001
        print("[serial error]", e)
        break
    if chunk:
        buf += chunk
        sys.stdout.write(chunk.decode("utf-8", errors="replace"))
        sys.stdout.flush()
ser.close()
print("\n[serial] closed")
