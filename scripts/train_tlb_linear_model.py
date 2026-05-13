#!/usr/bin/env python3
"""
Train the RISC-V TLB linear retention model from oracle CSV traces.

The trainer uses only features that the gem5 predictor currently consumes:

    deterministic_cost, walk_levels, is_small_page, missing_accessed,
    missing_dirty, writable, executable, user_page, log_bytes,
    log_residency, log_recency, log_access_count, reuse_density

It trains a logistic classifier with mini-batch SGD and prints equivalent raw
linear weights for riscv-fs.py. The model is trained on standardized features
internally, then converted back to raw feature space before printing.
"""

from __future__ import annotations

import argparse
import csv
import glob
import json
import math
from pathlib import Path
from typing import Iterable, List, Sequence, Tuple

import numpy as np


FEATURE_COLUMNS = [
    "deterministic_cost",
    "walk_levels",
    "is_small_page",
    "missing_accessed",
    "missing_dirty",
    "writable",
    "executable",
    "user_page",
    "log_bytes",
    "log_residency",
    "log_recency",
    "log_access_count",
    "reuse_density",
]

CSV_TO_FEATURE = {
    "deterministic_cost": "deterministic_cost",
    "walk_levels": "walk_levels",
    "is_small_page": "is_small_page",
    "missing_accessed": "pte_a",
    "missing_dirty": "pte_d",
    "writable": "pte_w",
    "executable": "pte_x",
    "user_page": "pte_u",
    "log_bytes": "log_bytes",
    "log_residency": "log_residency",
    "log_recency": "log_recency",
    "log_access_count": "log_access_count",
    "reuse_density": "reuse_density",
}


def find_default_csvs(root: Path) -> List[Path]:
    patterns = [
        root / "results" / "oracle_fft" / "*" / "dtb_oracle_trace.csv",
        root / "RADIX-*" / "**" / "dtb_oracle_trace.csv",
    ]
    paths: List[Path] = []
    for pattern in patterns:
        paths.extend(Path(path) for path in glob.glob(str(pattern), recursive=True))
    return sorted(set(paths))


def iter_csv_rows(paths: Sequence[Path]):
    for path in paths:
        with path.open(newline="") as handle:
            reader = csv.DictReader(handle)
            for row in reader:
                yield path, row


def row_to_example(row: dict) -> Tuple[int, np.ndarray]:
    if row.get("label") in (None, ""):
        raise ValueError("missing label")

    label = int(row["label"])
    features = []
    for feature in FEATURE_COLUMNS:
        csv_column = CSV_TO_FEATURE[feature]
        if row.get(csv_column) in (None, ""):
            raise ValueError(f"missing {csv_column}")
        value = float(row[csv_column])
        if feature == "missing_accessed":
            value = 0.0 if value else 1.0
        elif feature == "missing_dirty":
            value = 0.0 if value else 1.0
        features.append(value)
    return label, np.array(features, dtype=np.float64)


def first_pass(paths: Sequence[Path], max_rows: int | None = None):
    count = 0
    positives = 0
    sum_x = np.zeros(len(FEATURE_COLUMNS), dtype=np.float64)
    sum_x2 = np.zeros(len(FEATURE_COLUMNS), dtype=np.float64)

    skipped = 0

    for _path, row in iter_csv_rows(paths):
        try:
            label, x = row_to_example(row)
        except (KeyError, TypeError, ValueError):
            skipped += 1
            continue
        count += 1
        positives += label
        sum_x += x
        sum_x2 += x * x
        if max_rows and count >= max_rows:
            break

    if count == 0:
        raise RuntimeError("No training rows found")

    mean = sum_x / count
    var = np.maximum(sum_x2 / count - mean * mean, 1e-12)
    std = np.sqrt(var)
    negatives = count - positives
    return count, positives, negatives, mean, std, skipped


def batches(
    paths: Sequence[Path],
    mean: np.ndarray,
    std: np.ndarray,
    batch_size: int,
    max_rows: int | None = None,
):
    xs: List[np.ndarray] = []
    ys: List[int] = []
    count = 0
    for _path, row in iter_csv_rows(paths):
        try:
            label, x = row_to_example(row)
        except (KeyError, TypeError, ValueError):
            continue
        xs.append((x - mean) / std)
        ys.append(label)
        count += 1
        if len(xs) >= batch_size:
            yield np.vstack(xs), np.array(ys, dtype=np.float64)
            xs.clear()
            ys.clear()
        if max_rows and count >= max_rows:
            break

    if xs:
        yield np.vstack(xs), np.array(ys, dtype=np.float64)


def sigmoid(z: np.ndarray) -> np.ndarray:
    return 1.0 / (1.0 + np.exp(-np.clip(z, -40.0, 40.0)))


def train(
    paths: Sequence[Path],
    mean: np.ndarray,
    std: np.ndarray,
    positives: int,
    negatives: int,
    epochs: int,
    batch_size: int,
    lr: float,
    l2: float,
    max_rows: int | None = None,
):
    weights = np.zeros(len(FEATURE_COLUMNS), dtype=np.float64)
    bias = 0.0
    pos_weight = negatives / max(positives, 1)

    for epoch in range(epochs):
        total_loss = 0.0
        total_weight = 0.0
        total_correct = 0
        total_rows = 0

        for x_batch, y_batch in batches(paths, mean, std, batch_size, max_rows):
            logits = x_batch @ weights + bias
            probs = sigmoid(logits)
            sample_weights = np.where(y_batch > 0.5, pos_weight, 1.0)
            error = (probs - y_batch) * sample_weights
            denom = np.sum(sample_weights)

            grad_w = (x_batch.T @ error) / denom + l2 * weights
            grad_b = np.sum(error) / denom
            weights -= lr * grad_w
            bias -= lr * grad_b

            eps = 1e-9
            loss = -(
                y_batch * np.log(probs + eps)
                + (1.0 - y_batch) * np.log(1.0 - probs + eps)
            )
            total_loss += float(np.sum(loss * sample_weights))
            total_weight += float(denom)
            total_correct += int(np.sum((probs >= 0.5) == (y_batch > 0.5)))
            total_rows += len(y_batch)

        print(
            f"epoch={epoch + 1} loss={total_loss / total_weight:.6f} "
            f"accuracy={total_correct / total_rows:.4f}"
        )

    return weights, bias


def evaluate(
    paths: Sequence[Path],
    mean: np.ndarray,
    std: np.ndarray,
    weights: np.ndarray,
    bias: float,
    batch_size: int,
    max_rows: int | None = None,
):
    tp = fp = tn = fn = 0
    for x_batch, y_batch in batches(paths, mean, std, batch_size, max_rows):
        probs = sigmoid(x_batch @ weights + bias)
        pred = probs >= 0.5
        y = y_batch > 0.5
        tp += int(np.sum(pred & y))
        fp += int(np.sum(pred & ~y))
        tn += int(np.sum(~pred & ~y))
        fn += int(np.sum(~pred & y))

    precision = tp / max(tp + fp, 1)
    recall = tp / max(tp + fn, 1)
    accuracy = (tp + tn) / max(tp + fp + tn + fn, 1)
    return {
        "tp": tp,
        "fp": fp,
        "tn": tn,
        "fn": fn,
        "precision": precision,
        "recall": recall,
        "accuracy": accuracy,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", nargs="*", type=Path)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--epochs", type=int, default=5)
    parser.add_argument("--batch-size", type=int, default=65536)
    parser.add_argument("--lr", type=float, default=0.05)
    parser.add_argument("--l2", type=float, default=1e-4)
    parser.add_argument("--max-rows", type=int, default=None)
    parser.add_argument("--output", type=Path, default=Path("trained_tlb_linear_model.json"))
    args = parser.parse_args()

    paths = args.csv or find_default_csvs(args.root)
    paths = [path for path in paths if path.exists()]
    if not paths:
        raise SystemExit("No oracle CSV files found")

    print("training_csvs:")
    for path in paths:
        print(f"  {path}")

    count, positives, negatives, mean, std, skipped = first_pass(
        paths, args.max_rows
    )
    print(f"rows={count} positives={positives} negatives={negatives}")
    print(f"skipped_malformed_rows={skipped}")
    print(f"positive_rate={positives / count:.6f}")

    weights_std, bias_std = train(
        paths=paths,
        mean=mean,
        std=std,
        positives=positives,
        negatives=negatives,
        epochs=args.epochs,
        batch_size=args.batch_size,
        lr=args.lr,
        l2=args.l2,
        max_rows=args.max_rows,
    )

    metrics = evaluate(
        paths=paths,
        mean=mean,
        std=std,
        weights=weights_std,
        bias=bias_std,
        batch_size=args.batch_size,
        max_rows=args.max_rows,
    )

    raw_weights = weights_std / std
    raw_bias = bias_std - float(np.sum(weights_std * mean / std))

    model = {
        "features": FEATURE_COLUMNS,
        "linear_bias": raw_bias,
        "linear_threshold": 0.0,
        "linear_weights": raw_weights.tolist(),
        "standardized_bias": bias_std,
        "standardized_weights": weights_std.tolist(),
        "mean": mean.tolist(),
        "std": std.tolist(),
        "rows": count,
        "positives": positives,
        "negatives": negatives,
        "skipped_malformed_rows": skipped,
        "metrics": metrics,
        "csvs": [str(path) for path in paths],
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(model, indent=2))

    print("metrics:")
    for key, value in metrics.items():
        print(f"  {key}: {value}")

    print("Use these in riscv-fs.py:")
    print(f"ctrl.linear_bias = {raw_bias:.12g}")
    print("ctrl.linear_threshold = 0.0")
    print(
        "ctrl.linear_weights = ["
        + ", ".join(f"{weight:.12g}" for weight in raw_weights)
        + "]"
    )
    print(f"saved_model={args.output}")


if __name__ == "__main__":
    main()
