#!/usr/bin/env python3
"""
Phase 4 reproducibility checker.

Reads all JSON files from results/raw/ and computes, per
(category, benchmark, language, size):
  - mean, median, stdev, CV (coefficient of variation = stdev / mean)

Flags entries with CV above HIGH_CV_THRESHOLD (default 0.10 = 10%).
Writes a summary to results/processed/reproducibility.json.

Usage:
  python3 scripts/check_reproducibility.py [--threshold 0.10]
  python3 scripts/check_reproducibility.py --category numerical
"""

import argparse
import json
import math
import statistics
import sys
from pathlib import Path

BASE_DIR      = Path(__file__).resolve().parent.parent
RAW_DIR       = BASE_DIR / "results" / "raw"
PROCESSED_DIR = BASE_DIR / "results" / "processed"

HIGH_CV_THRESHOLD = 0.10   # 10% -- flag as potentially unreproducible


def load_raw_results(category_filter: "str | None") -> list:
    records = []
    if not RAW_DIR.exists():
        return records
    for path in sorted(RAW_DIR.glob("*.json")):
        try:
            with open(path) as f:
                data = json.load(f)
        except (json.JSONDecodeError, OSError):
            continue
        # Skip files missing required fields
        if not all(k in data for k in ("language", "category", "benchmark", "size", "timing_s")):
            continue
        if category_filter and data["category"] != category_filter:
            continue
        records.append(data)
    return records


def group_records(records: list) -> dict:
    """Group by (category, benchmark, language, size) -> list of timing dicts."""
    groups: dict = {}
    for r in records:
        key = (r["category"], r["benchmark"], r["language"], r["size"])
        groups.setdefault(key, []).append(r["timing_s"])
    return groups


def analyze_group(timing_list: list) -> dict:
    """Merge multiple timing dicts (from separate runs) into one analysis."""
    # Each timing dict has 'runs' (list of per-iteration times) + summary fields.
    all_runs: list[float] = []
    for t in timing_list:
        if isinstance(t, dict) and "runs" in t:
            all_runs.extend(t["runs"])
        elif isinstance(t, (int, float)):
            all_runs.append(float(t))

    if not all_runs:
        return {"n": 0, "mean": None, "median": None, "stdev": None, "cv": None}

    n = len(all_runs)
    mean   = statistics.mean(all_runs)
    median = statistics.median(all_runs)
    stdev  = statistics.stdev(all_runs) if n > 1 else 0.0
    cv     = stdev / mean if mean != 0 else None

    return {
        "n":      n,
        "mean":   round(mean,   6),
        "median": round(median, 6),
        "stdev":  round(stdev,  6),
        "cv":     round(cv, 4) if cv is not None else None,
        "min":    round(min(all_runs), 6),
        "max":    round(max(all_runs), 6),
    }


def main() -> None:
    ap = argparse.ArgumentParser(description="Phase 4 reproducibility checker")
    ap.add_argument(
        "--threshold", type=float, default=HIGH_CV_THRESHOLD,
        help=f"CV threshold for flagging (default: {HIGH_CV_THRESHOLD})",
    )
    ap.add_argument(
        "--category", default=None,
        help="Filter to a single category",
    )
    args = ap.parse_args()

    records = load_raw_results(args.category)
    if not records:
        print(f"No result files found in {RAW_DIR}")
        print("Run scripts/run_all.sh first to collect benchmark data.")
        sys.exit(0)

    groups = group_records(records)

    print(f"=== Phase 4 Reproducibility Check  (threshold CV>{args.threshold:.0%}) ===\n")
    print(f"Loaded {len(records)} result files, {len(groups)} unique benchmark groups.\n")

    flagged = []
    summary_rows = []

    for (cat, bench, lang, size), timing_list in sorted(groups.items()):
        stats = analyze_group(groups[(cat, bench, lang, size)])
        cv = stats["cv"]
        flag = cv is not None and cv > args.threshold

        row = {
            "category":  cat,
            "benchmark": bench,
            "language":  lang,
            "size":      size,
            **stats,
            "flagged":   flag,
        }
        summary_rows.append(row)

        status = "FLAG" if flag else "ok  "
        cv_str = f"{cv:.1%}" if cv is not None else "  n/a"
        # mean/stdev are None when every raw record for this group is an
        # absent/failed/timed-out status JSON (write_status_json's
        # timing_s: null) -- the key is present, so load_raw_results' schema
        # check doesn't filter it out, but there is no timing to report.
        mean_str  = f"{stats['mean']:.4f}s"  if stats['mean']  is not None else "n/a   "
        stdev_str = f"{stats['stdev']:.4f}s" if stats['stdev'] is not None else "n/a   "
        print(
            f"  [{status}] {cat}/{bench} {lang:8s} {size:6s}"
            f"  mean={mean_str}  stdev={stdev_str}  CV={cv_str}"
            f"  n={stats['n']}"
        )
        if flag:
            flagged.append(row)

    print(f"\n=== {len(flagged)} entries flagged (CV > {args.threshold:.0%}) ===")
    if flagged:
        for r in flagged:
            cv_str = f"{r['cv']:.1%}" if r['cv'] is not None else "n/a"
            print(f"  {r['category']}/{r['benchmark']}  lang={r['language']}  size={r['size']}  CV={cv_str}")

    PROCESSED_DIR.mkdir(parents=True, exist_ok=True)
    out_path = PROCESSED_DIR / "reproducibility.json"
    with open(out_path, "w") as f:
        json.dump(
            {
                "threshold": args.threshold,
                "total_groups": len(groups),
                "flagged_count": len(flagged),
                "entries": summary_rows,
            },
            f, indent=2,
        )
    print(f"\nReport written to: {out_path}")


if __name__ == "__main__":
    main()
