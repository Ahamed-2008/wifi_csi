"""Validate and prepare labeled ESP32 CSI captures for feature extraction."""

import argparse
import csv
import json
from collections import Counter
from pathlib import Path

import numpy as np


EXPECTED_CSI_LEN = 384
EXPECTED_COLUMNS = {"timestamp_ms", "rssi", "csi_len", "csi_bytes", "label"}


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-dir", type=Path, default=Path("data"))
    parser.add_argument("--output", type=Path, default=Path("data/processed_csi.npz"))
    parser.add_argument("--report", type=Path, default=Path("data/preprocessing_report.json"))
    return parser.parse_args()


def parse_row(row):
    """Return parsed metadata and CSI values, or a reason for rejection."""
    if set(row) != EXPECTED_COLUMNS or any(value is None for value in row.values()):
        return None, "column_structure"

    try:
        timestamp_ms = int(row["timestamp_ms"])
        rssi = int(row["rssi"])
        csi_len = int(row["csi_len"])
        values = [int(value) for value in row["csi_bytes"].split(";") if value != ""]
    except ValueError:
        return None, "numeric_parse"

    if timestamp_ms < 0:
        return None, "negative_timestamp"
    if csi_len != EXPECTED_CSI_LEN:
        return None, "unsupported_csi_length"
    if len(values) != csi_len:
        return None, "payload_length_mismatch"

    return (timestamp_ms, rssi, row["label"], values), None


def preprocess(input_dir):
    rows = []
    report = {"files": {}, "totals": Counter()}

    for path in sorted(input_dir.glob("*.csv")):
        file_report = Counter()
        with path.open(newline="") as capture:
            reader = csv.DictReader(capture)
            for row in reader:
                parsed, reason = parse_row(row)
                if reason:
                    file_report[reason] += 1
                    continue
                timestamp_ms, rssi, label, values = parsed
                rows.append((path.stem, timestamp_ms, rssi, label, values))
                file_report["accepted"] += 1

        report["files"][path.name] = dict(file_report)
        report["totals"].update(file_report)

    report["totals"] = dict(report["totals"])
    return rows, report


def save_dataset(rows, output_path):
    output_path.parent.mkdir(parents=True, exist_ok=True)
    if not rows:
        raise ValueError("No valid CSI rows were found")

    session_ids, timestamps, rssis, labels, csi_values = zip(*rows)
    np.savez_compressed(
        output_path,
        session_id=np.asarray(session_ids),
        timestamp_ms=np.asarray(timestamps, dtype=np.int64),
        rssi=np.asarray(rssis, dtype=np.int16),
        label=np.asarray(labels),
        csi=np.asarray(csi_values, dtype=np.int16),
    )


def main():
    args = parse_args()
    rows, report = preprocess(args.input_dir)
    save_dataset(rows, args.output)

    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n")

    totals = report["totals"]
    print(f"Accepted: {totals.get('accepted', 0)} rows")
    print(f"Rejected: {sum(value for key, value in totals.items() if key != 'accepted')} rows")
    print(f"Dataset: {args.output}")
    print(f"Report: {args.report}")


if __name__ == "__main__":
    main()