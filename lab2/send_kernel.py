#!/usr/bin/env python3
import argparse
import struct
import sys
import time

MAGIC = 0x544F4F42  # "BOOT"


def checksum_u32(data: bytes) -> int:
    return sum(data) & 0xFFFFFFFF


def main() -> int:
    parser = argparse.ArgumentParser(description="Send kernel.bin to lab2 load command over UART")
    parser.add_argument("port", help="Serial device path, e.g. /dev/ttyUSB0 or /dev/pts/5")
    parser.add_argument("image", help="Kernel image to send")
    parser.add_argument("--delay", type=float, default=0.05,
                        help="Delay between header and payload in seconds (default: 0.05)")
    args = parser.parse_args()

    try:
        with open(args.image, "rb") as f:
            payload = f.read()
    except OSError as e:
        print(f"failed to read image: {e}", file=sys.stderr)
        return 1

    size = len(payload)
    csum = checksum_u32(payload)
    header = struct.pack("<III", MAGIC, size, csum)

    print(f"sending: magic=0x{MAGIC:08x} size={size} checksum=0x{csum:08x}")

    try:
        with open(args.port, "wb", buffering=0) as tty:
            tty.write(header)
            if args.delay > 0:
                time.sleep(args.delay)
            tty.write(payload)
    except OSError as e:
        print(f"failed to write to serial port: {e}", file=sys.stderr)
        return 1

    print("done")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
