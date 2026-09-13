"""
csi_capture.py

Reads CSI CSV rows streamed over Serial from csi_receiver.ino and logs them
to a CSV file on the laptop, tagged with a session label (e.g. "empty",
"occupied", "motion") for later use in training the classifier.

Expected incoming serial line format from the receiver:
    timestamp_ms,rssi,csi_len,csi_bytes

Lines that don't match this format (e.g. startup messages, the
"[debug] saw MAC: ..." lines, or the CSV header) are skipped automatically.

Usage:
    python csi_capture.py --port COM5 --label empty
    python csi_capture.py --port /dev/ttyUSB0 --label motion --baud 115200

Output:
    Appends rows to data/<label>_<timestamp>.csv in the current directory,
    with an added 'label' column so multiple sessions can later be combined
    into one training dataset.
"""

import argparse
import csv
import os
import re
import sys
import time
from datetime import datetime

import serial  # pyserial - install with: pip install pyserial

# A valid data row looks like: 12345,-76,128,30;-32;1;0;...
# (timestamp, rssi, csi_len, then csi_bytes - all comma-separated at the top level)
ROW_PATTERN = re.compile(r"^\d+,-?\d+,\d+,")


def parse_args():
    parser = argparse.ArgumentParser(description="Capture CSI CSV rows from ESP32 receiver over Serial.")
    parser.add_argument("--port", required=True, help="Serial port, e.g. COM5 or /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=921600, help="Baud rate (default: 921600)")
    parser.add_argument("--label", required=True, choices=["empty", "occupied", "motion"],
                         help="Room state label for this capture session")
    parser.add_argument("--outdir", default="data", help="Output directory (default: ./data)")
    parser.add_argument("--duration", type=int, default=None,
                         help="Optional: auto-stop after N seconds (otherwise Ctrl+C to stop)")
    return parser.parse_args()


def main():
    args = parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    session_stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    outfile_path = os.path.join(args.outdir, f"{args.label}_{session_stamp}.csv")

    print(f"Opening serial port {args.port} at {args.baud} baud...")
    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except serial.SerialException as e:
        print(f"ERROR: Could not open serial port: {e}")
        sys.exit(1)

    # Let the ESP32 finish its boot/reset sequence before we start expecting data
    time.sleep(2)

    print(f"Logging to {outfile_path}")
    print(f"Label for this session: {args.label}")
    print("Press Ctrl+C to stop capturing.\n")

    row_count = 0
    start_time = time.time()

    with open(outfile_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["timestamp_ms", "rssi", "csi_len", "csi_bytes", "label"])

        try:
            while True:
                if args.duration and (time.time() - start_time) >= args.duration:
                    print(f"\nReached duration limit of {args.duration}s. Stopping.")
                    break

                raw_line = ser.readline()
                if not raw_line:
                    continue  # timeout with no data, just loop again

                try:
                    line = raw_line.decode("utf-8", errors="ignore").strip()
                except UnicodeDecodeError:
                    continue  # skip garbled lines

                if not line:
                    continue

                # Skip anything that isn't a real CSI data row
                # (startup messages, "[debug] saw MAC: ..." lines, the CSV header, etc.)
                if not ROW_PATTERN.match(line):
                    continue

                parts = line.split(",", 3)  # timestamp, rssi, csi_len, csi_bytes
                if len(parts) < 4:
                    continue  # malformed row, skip

                timestamp_ms, rssi, csi_len, csi_bytes = parts
                writer.writerow([timestamp_ms, rssi, csi_len, csi_bytes, args.label])
                row_count += 1

                if row_count % 50 == 0:
                    elapsed = time.time() - start_time
                    print(f"  {row_count} rows captured ({elapsed:.1f}s elapsed)")

        except KeyboardInterrupt:
            print("\nStopped by user.")

    ser.close()
    print(f"\nDone. {row_count} rows saved to {outfile_path}")


if __name__ == "__main__":
    main()