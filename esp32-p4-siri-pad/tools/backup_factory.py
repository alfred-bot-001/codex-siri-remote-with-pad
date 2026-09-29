"""Read the exact P4 factory app in restartable 64 KiB chunks before replacing it."""
import argparse
import hashlib
import pathlib
import subprocess
import sys
import tempfile

from serial.tools import list_ports

CHUNK = 0x10000
EXPECTED_USB_SERIAL = "5B90124240"
EXPECTED_MAC = "80:f1:b2:d5:c8:7f"

parser = argparse.ArgumentParser()
parser.add_argument("--port", required=True)
parser.add_argument("--output", type=pathlib.Path, required=True)
parser.add_argument("--offset", type=lambda value: int(value, 0), default=0x200000)
parser.add_argument("--size", type=lambda value: int(value, 0), default=0x800000)
args = parser.parse_args()
if args.offset % CHUNK or args.size % CHUNK or args.offset < 0 or args.offset + args.size > 0x2000000:
    raise SystemExit("Backup range must be 64 KiB aligned and within the 32 MiB flash")

info = next((p for p in list_ports.comports() if p.device == args.port), None)
if not info or (info.vid, info.pid, info.serial_number) != (0x1A86, 0x55D3, EXPECTED_USB_SERIAL):
    raise SystemExit("Expected ESP32-P4 USB TO UART port not found; no flash was read")

args.output.parent.mkdir(parents=True, exist_ok=True)
done = args.output.stat().st_size if args.output.exists() else 0
if done % CHUNK or done > args.size:
    raise SystemExit("Existing backup size is not aligned to a 64 KiB chunk")

with args.output.open("ab") as output:
    for position in range(done, args.size, CHUNK):
        with tempfile.TemporaryDirectory() as temp:
            piece = pathlib.Path(temp) / "piece.bin"
            command = [sys.executable, "-m", "esptool", "--chip", "esp32p4", "--port", args.port,
                       "--baud", "115200", "read-flash", hex(args.offset + position), hex(CHUNK), str(piece)]
            for attempt in range(1, 5):
                result = subprocess.run(command, text=True, capture_output=True, timeout=45)
                if result.returncode == 0 and EXPECTED_MAC in result.stdout.lower() and piece.exists() and piece.stat().st_size == CHUNK:
                    break
                print(f"Retry chunk 0x{position:x}: attempt {attempt}", flush=True)
            else:
                raise SystemExit(f"Could not safely back up chunk 0x{position:x}: {result.stdout[-500:]} {result.stderr[-500:]}")
            output.write(piece.read_bytes())
            output.flush()
            if (position + CHUNK) % 0x100000 == 0:
                print(f"Backed up {(position + CHUNK) // 0x100000} / {args.size // 0x100000} MiB", flush=True)

digest = hashlib.sha256(args.output.read_bytes()).hexdigest()
print(f"Factory app backup complete: {args.output} SHA-256 {digest}", flush=True)
