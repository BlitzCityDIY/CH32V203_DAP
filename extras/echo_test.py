#!/usr/bin/env python3
"""Host test: verify CDC echo integrity and measure throughput.

Also tests the cmsis_dap_probe UART bridge: put a jumper from the QT Py's
TX pin to its RX pin, then e.g.
    python echo_test.py COM10 --baud 115200 --bytes 16384
    python echo_test.py COM10 --baud 1000000 --bytes 131072

Usage:
    pip install pyserial
    python echo_test.py /dev/ttyACM0            # Linux
    python echo_test.py /dev/cu.usbmodemXXXX    # macOS
    python echo_test.py COM7                    # Windows
    python echo_test.py PORT --bytes 262144 --chunk 64

Prints the board's diagnostics banner, then streams random data through
the echo loop and reports round-trip throughput plus any mismatches.
"""

import argparse
import os
import sys
import threading
import time

import serial

READY = b"--- echo ready ---"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--bytes", type=int, default=128 * 1024,
                    help="total bytes to send (default 128 KiB)")
    ap.add_argument("--chunk", type=int, default=64,
                    help="bytes per write (default 64)")
    ap.add_argument("--baud", type=int, default=115200,
                    help="baud rate (USB-only echo ignores it; the UART "
                         "bridge applies it to the target UART)")
    args = ap.parse_args()

    # Open with DTR held low, then raise it ourselves. On Windows, pyserial
    # raises DTR during open() and then purges the receive buffer, which
    # throws away the banner the board sends in response to DTR. Opening
    # with DTR low also guarantees a fresh low->high edge every run.
    ser = serial.serial_for_url(args.port, do_not_open=True,
                                baudrate=args.baud, timeout=0.5)
    ser.dtr = False
    ser.open()
    time.sleep(0.1)
    ser.reset_input_buffer()
    ser.dtr = True  # the sketch prints its banner on DTR rising edge

    # Read the banner if one arrives, but don't depend on it: the echo test
    # still runs so we can tell "banner missing" apart from "USB TX dead".
    banner = b""
    deadline = time.time() + 3
    while READY not in banner and time.time() < deadline:
        banner += ser.read(256)
    sys.stdout.write(banner.decode(errors="replace"))
    if READY in banner:
        time.sleep(0.2)
    else:
        print(f"\n[warning] no complete banner ({len(banner)} bytes received); "
              "running echo test anyway.")
        print("          Check the NeoPixel: blinking green = sketch alive, "
              "frozen = sketch hung.")
    ser.reset_input_buffer()

    payload = os.urandom(args.bytes)
    received = bytearray()

    def reader():
        while len(received) < len(payload):
            data = ser.read(4096)
            if not data and time.time() - t0 > 30:
                break
            received.extend(data)

    print(f"\nSending {args.bytes} bytes in {args.chunk}-byte writes...")
    t0 = time.time()
    th = threading.Thread(target=reader, daemon=True)
    th.start()
    for i in range(0, len(payload), args.chunk):
        ser.write(payload[i:i + args.chunk])
    th.join(timeout=60)
    elapsed = time.time() - t0

    if not received:
        print("No echo data at all. If the NeoPixel is still blinking green, "
              "the sketch is alive but USB transmit is stuck.")
    ok = bytes(received) == payload
    print(f"Received {len(received)} / {len(payload)} bytes in {elapsed:.2f} s")
    print(f"Round-trip throughput: {len(received) / elapsed / 1024:.1f} KiB/s")
    if ok:
        print("PASS: echo data matches")
    else:
        first = next((i for i, (a, b) in enumerate(zip(received, payload))
                      if a != b), min(len(received), len(payload)))
        print(f"FAIL: first difference at byte offset {first}")
    ser.close()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
