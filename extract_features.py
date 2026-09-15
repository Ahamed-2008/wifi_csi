"""Extract fixed-window CSI features from the validated NumPy dataset."""

import argparse
import json
from pathlib import Path

import numpy as np


DEFAULT_WINDOW_SIZE = 50
DEFAULT_STEP = 25
DEFAULT_MAX_GAP_MS = 250


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=Path("data/processed_csi.npz"))
    parser.add_argument("--output", type=Path, default=Path("data/csi_features.npz"))
    parser.add_argument("--window-size", type=int, default=DEFAULT_WINDOW_SIZE)
    parser.add_argument("--step", type=int, default=DEFAULT_STEP)
    parser.add_argument("--max-gap-ms", type=int, default=DEFAULT_MAX_GAP_MS)
    return parser.parse_args()


def feature_names(subcarrier_count):
    names = ["rssi_mean", "rssi_std", "rssi_min", "rssi_max", "rssi_diff_std"]
    names.extend(f"amplitude_mean_{index}" for index in range(subcarrier_count))
    names.extend(f"amplitude_std_{index}" for index in range(subcarrier_count))
    names.extend(f"amplitude_diff_std_{index}" for index in range(subcarrier_count))
    names.extend(f"amplitude_abs_diff_mean_{index}" for index in range(subcarrier_count))
    return names


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


def extract_features(dataset, window_size, step, max_gap_ms):
    csi = dataset["csi"]
    if csi.ndim != 2 or csi.shape[1] % 2:
        raise ValueError("Expected a 2D interleaved I/Q CSI array with an even width")
    if window_size < 2 or step < 1:
        raise ValueError("window-size must be at least 2 and step must be positive")

    amplitude = np.hypot(csi[:, 0::2], csi[:, 1::2]).astype(np.float32)
    session_ids = dataset["session_id"]
    timestamps = dataset["timestamp_ms"]
    labels = dataset["label"]
    features = []
    window_labels = []
    window_sessions = []
    window_starts = []

    start = 0
    while start + window_size <= len(csi):
        end = start + window_size
        same_session = np.all(session_ids[start:end] == session_ids[start])
        gaps = np.diff(timestamps[start:end])
        continuous = len(gaps) == 0 or np.all(gaps <= max_gap_ms)
        same_label = np.all(labels[start:end] == labels[start])

        if same_session and continuous and same_label:
            features.append(window_features(dataset["rssi"][start:end], amplitude[start:end]))
            window_labels.append(labels[start])
            window_sessions.append(session_ids[start])
            window_starts.append(timestamps[start])
            start += step
        else:
            start += 1

    if not features:
        raise ValueError("No valid windows were found")

    return (
        np.asarray(features, dtype=np.float32),
        np.asarray(window_labels),
        np.asarray(window_sessions),
        np.asarray(window_starts, dtype=np.int64),
        feature_names(amplitude.shape[1]),
    )


def main():
    args = parse_args()
    if args.max_gap_ms < 1:
        raise ValueError("max-gap-ms must be positive")

    with np.load(args.input) as dataset:
        features, labels, sessions, starts, names = extract_features(
            dataset, args.window_size, args.step, args.max_gap_ms
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(
        args.output,
        features=features,
        label=labels,
        session_id=sessions,
        start_timestamp_ms=starts,
    )
    metadata = {
        "input": str(args.input),
        "window_size": args.window_size,
        "step": args.step,
        "window_duration_ms_approx": args.window_size * 100,
        "max_gap_ms": args.max_gap_ms,
        "feature_count": len(names),
        "feature_names": names,
    }
    metadata_path = args.output.with_suffix(".json")
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")

    unique_labels, counts = np.unique(labels, return_counts=True)
    print(f"Windows: {len(features)}")
    print(f"Features per window: {features.shape[1]}")
    print(f"Class counts: {dict(zip(unique_labels.tolist(), counts.tolist()))}")
    print(f"Dataset: {args.output}")
    print(f"Metadata: {metadata_path}")


if __name__ == "__main__":
    main()