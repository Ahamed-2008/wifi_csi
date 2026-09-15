"""Train and evaluate a Random Forest using session-level CSI splits."""

import argparse
import json
from pathlib import Path

import joblib
import numpy as np
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import classification_report, confusion_matrix, accuracy_score


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=Path("data/csi_features.npz"))
    parser.add_argument("--metadata", type=Path, default=Path("data/csi_features.json"))
    parser.add_argument("--model", type=Path, default=Path("data/random_forest.joblib"))
    parser.add_argument("--report", type=Path, default=Path("data/training_report.json"))
    parser.add_argument("--trees", type=int, default=300)
    return parser.parse_args()


def choose_sessions(labels, sessions):
    """Hold out one deterministic complete session for every class."""
    test_sessions = {}
    for label in np.unique(labels):
        candidates = sorted(set(sessions[labels == label].tolist()))
        if len(candidates) < 2:
            raise ValueError(f"Need at least two sessions for label {label!r}")
        test_sessions[label] = candidates[-1]

    test_session_ids = set(test_sessions.values())
    test_mask = np.isin(sessions, list(test_session_ids))
    if np.any(labels[test_mask] == "") or len(np.unique(labels[test_mask])) < 3:
        raise ValueError("Selected holdout does not contain all classes")
    return test_mask, test_sessions


def main():
    args = parse_args()
    if args.trees < 1:
        raise ValueError("trees must be positive")

    with np.load(args.input) as dataset:
        features = dataset["features"]
        labels = dataset["label"]
        sessions = dataset["session_id"]

    test_mask, test_sessions = choose_sessions(labels, sessions)
    train_mask = ~test_mask
    if set(sessions[train_mask]) & set(sessions[test_mask]):
        raise AssertionError("Session leakage detected")

    model = RandomForestClassifier(
        n_estimators=args.trees,
        class_weight="balanced",
        random_state=42,
        n_jobs=-1,
    )
    model.fit(features[train_mask], labels[train_mask])
    predictions = model.predict(features[test_mask])
    class_names = sorted(np.unique(labels).tolist())

    report = classification_report(
        labels[test_mask], predictions, labels=class_names, output_dict=True, zero_division=0
    )
    report_data = {
        "input": str(args.input),
        "trees": args.trees,
        "random_state": 42,
        "train_sessions": sorted(set(sessions[train_mask].tolist())),
        "test_sessions": test_sessions,
        "train_windows": int(train_mask.sum()),
        "test_windows": int(test_mask.sum()),
        "accuracy": float(accuracy_score(labels[test_mask], predictions)),
        "classes": class_names,
        "confusion_matrix": confusion_matrix(labels[test_mask], predictions, labels=class_names).tolist(),
        "classification_report": report,
    }

    args.model.parent.mkdir(parents=True, exist_ok=True)
    joblib.dump(
        {
            "model": model,
            "feature_count": features.shape[1],
            "classes": class_names,
            "metadata": str(args.metadata),
        },
        args.model,
    )
    args.report.write_text(json.dumps(report_data, indent=2) + "\n")

    print(f"Train windows: {train_mask.sum()}")
    print(f"Test windows: {test_mask.sum()}")
    print(f"Test sessions: {test_sessions}")
    print(f"Accuracy: {report_data['accuracy']:.3f}")
    print(f"Model: {args.model}")
    print(f"Report: {args.report}")
    print(classification_report(labels[test_mask], predictions, labels=class_names, zero_division=0))


if __name__ == "__main__":
    main()