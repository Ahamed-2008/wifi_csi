"""Run live CSI inference and send the predicted state to the receiver LCD."""

import argparse
import re
import time
from collections import deque
from pathlib import Path

import joblib
import numpy as np
import serial

WINDOW_SIZE = 50
STEP = 25
MAX_GAP_MS = 250
EXPECTED_CSI_LEN = 384
ROW_PATTERN = re.compile(r"^\d+,-?\d+,\d+,")


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Receiver serial port")
    parser.add_argument("--baud", type=int, default=921600)
    parser.add_argument("--model", type=Path, default=Path("data/random_forest.joblib"))
    return parser.parse_args()


def window_features(rssi, amplitude):
    differences = np.diff(amplitude, axis=0)
    scalar_features = np.array(
        [
            rssi.mean(),
            rssi.std(),
            rssi.min(),
            rssi.max(),
            np.diff(rssi).std(),
        ],
        dtype=np.float32,
    )
    return np.concatenate(
        [
            scalar_features,
            amplitude.mean(axis=0),
            amplitude.std(axis=0),
            differences.std(axis=0),
            np.abs(differences).mean(axis=0),
        ]
    ).astype(np.float32)


def parse_csi_line(line):
    if not ROW_PATTERN.match(line):
        return None
    try:
        parts = line.split(",", 3)
        if len(parts) < 4:
            return None
        timestamp, rssi, csi_len, raw_values = parts
        values = [int(value) for value in raw_values.split(";") if value]
        if int(csi_len) != EXPECTED_CSI_LEN or len(values) != EXPECTED_CSI_LEN:
            return None
        return int(timestamp), int(rssi), np.asarray(values, dtype=np.float32)
    except (ValueError, StopIteration):
        return None


def main():
    args = parse_args()
    bundle = joblib.load(args.model)
    model = bundle["model"]
    if bundle["feature_count"] != 773:
        raise ValueError(f"Expected 773 model features, found {bundle['feature_count']}")

    frames = deque(maxlen=WINDOW_SIZE)
    serial_port = serial.Serial(args.port, args.baud, timeout=1)
    time.sleep(2)
    serial_port.write(b"LCD:waiting\n")
    serial_port.flush()
    print(f"Listening on {args.port} at {args.baud} baud")
    print("LCD test sent. Move your hand near the sensing path. Press Ctrl+C to stop.")

    predictions = deque(maxlen=3)
    last_prediction = None
    valid_frames = 0
    rejected_lines = 0
    gap_resets = 0
    try:
        while True:
            raw_line = serial_port.readline()
            if not raw_line:
                continue
            parsed = parse_csi_line(raw_line.decode("utf-8", errors="ignore").strip())
            if parsed is None:
                rejected_lines += 1
                continue
            valid_frames += 1
            if valid_frames % 50 == 0:
                print(f"Received {valid_frames} valid CSI frames ({rejected_lines} ignored lines)")
            timestamp, rssi, values = parsed
            amplitude = np.hypot(values[0::2], values[1::2])

            if frames and timestamp - frames[-1][0] > MAX_GAP_MS:
                frames.clear()
                gap_resets += 1
                print(f"CSI gap exceeded {MAX_GAP_MS} ms; restarting window ({gap_resets})")
            frames.append((timestamp, rssi, amplitude))
            if len(frames) < WINDOW_SIZE:
                continue

            rssis = np.asarray([frame[1] for frame in frames], dtype=np.float32)
            amplitudes = np.asarray([frame[2] for frame in frames], dtype=np.float32)
            features = window_features(rssis, amplitudes)
            label = str(model.predict(features.reshape(1, -1))[0])
            predictions.append(label)
            stable_label = max(set(predictions), key=predictions.count)

            if stable_label != last_prediction:
                serial_port.write(f"LCD:{stable_label}\n".encode("ascii"))
                serial_port.flush()
                print(f"{time.strftime('%H:%M:%S')}  {stable_label}  RSSI={parsed[1]} ")
                last_prediction = stable_label

            for _ in range(STEP - 1):
                if frames:
                    frames.popleft()
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        serial_port.close()


if __name__ == "__main__":
    main()
