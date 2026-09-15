"""Evaluate the CSI Random Forest with one complete session held out per fold."""

import argparse
import json
from pathlib import Path

import numpy as np
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import accuracy_score, classification_report, confusion_matrix


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=Path("data/csi_features.npz"))
    parser.add_argument("--output", type=Path, default=Path("data/cross_validation_report.json"))
    parser.add_argument("--trees", type=int, default=300)
    return parser.parse_args()


def main():
    args = parse_args()
    with np.load(args.input) as dataset:
        features = dataset["features"]
        labels = dataset["label"]
        sessions = dataset["session_id"]

    class_names = sorted(np.unique(labels).tolist())
    fold_results = []
    all_actual = []
    all_predictions = []

    for session in sorted(np.unique(sessions).tolist()):
        test_mask = sessions == session
        train_mask = ~test_mask
        model = RandomForestClassifier(
            n_estimators=args.trees,
            class_weight="balanced",
            random_state=42,
            n_jobs=-1,
        )
        model.fit(features[train_mask], labels[train_mask])
        predictions = model.predict(features[test_mask])
        actual = labels[test_mask]
        all_actual.extend(actual.tolist())
        all_predictions.extend(predictions.tolist())
        fold_results.append(
            {
                "session": session,
                "label": np.unique(actual).tolist()[0],
                "windows": int(test_mask.sum()),
                "accuracy": float(accuracy_score(actual, predictions)),
                "confusion_matrix": confusion_matrix(
                    actual, predictions, labels=class_names
                ).tolist(),
            }
        )

    overall_report = classification_report(
        all_actual, all_predictions, labels=class_names, output_dict=True, zero_division=0
    )
    report = {
        "input": str(args.input),
        "trees": args.trees,
        "random_state": 42,
        "sessions": len(fold_results),
        "total_windows": len(all_actual),
        "mean_session_accuracy": float(np.mean([fold["accuracy"] for fold in fold_results])),
        "overall_accuracy": float(accuracy_score(all_actual, all_predictions)),
        "classes": class_names,
        "folds": fold_results,
        "overall_confusion_matrix": confusion_matrix(
            all_actual, all_predictions, labels=class_names
        ).tolist(),
        "classification_report": overall_report,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")

    print(f"Sessions: {report['sessions']}")
    print(f"Mean session accuracy: {report['mean_session_accuracy']:.3f}")
    print(f"Overall accuracy: {report['overall_accuracy']:.3f}")
    for fold in fold_results:
        print(f"{fold['session']}: {fold['accuracy']:.3f} ({fold['windows']} windows)")
    print(f"Report: {args.output}")


if __name__ == "__main__":
    main()