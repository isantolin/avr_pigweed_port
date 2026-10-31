#!/usr/bin/env python3
"""Script to receive and decode telemetry sent by the AVR firmware using Pigweed."""

import sys
import argparse
import serial
from pw_hdlc import decode
from pw_varint import decode as varint_decode

def parse_args():
    parser = argparse.ArgumentParser(description="Pigweed HDLC telemetry receiver for AVR")
    parser.add_argument("-p", "--port", default="/dev/ttyACM0", help="Serial port (default: /dev/ttyACM0)")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    return parser.parse_args()

def main():
    args = parse_args()
    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except serial.SerialException as e:
        print(f"Error opening port {args.port}: {e}", file=sys.stderr)
        sys.exit(1)

    decoder = decode.FrameDecoder()
    print(f"Listening for pw_hdlc frames on {args.port} at {args.baud} baud...")

    try:
        while True:
            raw_bytes = ser.read(64)
            if not raw_bytes:
                continue
            for frame in decoder.process_valid_frames(raw_bytes):
                data = frame.data
                try:
                    # Decode varints: sequence and adc_sample
                    seq, seq_len = varint_decode.decode(data)
                    adc, _ = varint_decode.decode(data[seq_len:])
                    print(f"[HDLC OK] Addr: {frame.address} | Sequence: {seq:5d} | ADC0: {adc:4d} | Raw: {data.hex()}")
                except Exception:
                    print(f"[HDLC OK] Addr: {frame.address} | Payload (hex): {data.hex()}")
    except KeyboardInterrupt:
        print("\nTerminated by user.")
    finally:
        ser.close()

if __name__ == "__main__":
    main()
