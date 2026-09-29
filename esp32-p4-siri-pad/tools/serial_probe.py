"""Read diagnostics from the verified P4 UART, with a startup grace period for UART adapters that reset on open."""
import argparse
import time
from pathlib import Path
import serial
from serial.tools import list_ports

p=argparse.ArgumentParser()
p.add_argument('--port',default='/dev/cu.usbmodem5B901242401')
p.add_argument('--seconds',type=float,default=15)
p.add_argument('--command',default='status')
p.add_argument('--reset',action='store_true')
p.add_argument('--log',type=Path,required=True)
a=p.parse_args()
info=next((x for x in list_ports.comports() if x.device==a.port),None)
if not info or (info.vid,info.pid,info.serial_number)!=(0x1a86,0x55d3,'5B90124240'):
    raise SystemExit('Verified P4 UART not present')
s=serial.Serial(port=None,baudrate=115200,timeout=0.2)
s.dtr=False;s.rts=False;s.port=a.port;s.open()
if a.reset:
    s.rts=True;time.sleep(0.15);s.rts=False
end=time.monotonic()+a.seconds
sent=False
with a.log.open('wb') as f:
    while time.monotonic()<end:
        if not sent and time.monotonic()>end-a.seconds+6:
            s.write(('\n'+a.command+'\n').encode());sent=True
        data=s.read(4096)
        if data:f.write(data);f.flush()
s.close()
print(a.log)
