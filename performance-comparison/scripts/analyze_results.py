#!/usr/bin/env python3
"""
Phase 4 results analyzer.

Aggregates all JSON files in results/raw/, then:
  1. Computes speedup ratios relative to the C baseline (mean time).
  2. Identifies outlier runs (individual runs > mean + 3*stdev within a group).
  3. Writes processed results to:
       results/processed/summary.json       -- all benchmark groups with stats + speedups
       results/processed/by_category.json   -- grouped by category
       results/processed/outliers.json      -- individual runs flagged as outliers

Usage:
  python3 scripts/analyze_results.py
  python3 scripts/analyze_results.py --category numerical
  python3 scripts/analyze_results.py --print-summary
"""

import argparse
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path

BASE_DIR      = Path(__file__).resolve().parent.parent
RAW_DIR       = BASE_DIR / "results" / "raw"
PROCESSED_DIR = BASE_DIR / "results" / "processed"

OUTLIER_SIGMA = 3.0   # flag runs more than N stdev above the mean


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
        required = ("language", "category", "benchmark", "size", "timing_s")
        if not all(k in data for k in required):
            continue
        if category_filter and data["category"] != category_filter:
            continue
        records.append(data)
    return records


def aggregate(records: list) -> dict:
    """
    Group records by (category, benchmark, size) -> {lang -> merged timing stats}.
    When multiple JSON files exist for the same (cat, bench, lang, size), merge runs.
    """
    # key: (cat, bench, size, lang) -> list of runs (floats)
    all_runs: dict = defaultdict(list)

    for r in records:
        key = (r["category"], r["benchmark"], r["size"], r["language"])
        t = r["timing_s"]
        if isinstance(t, dict) and "runs" in t:
            all_runs[key].extend(t["runs"])
        elif isinstance(t, (int, float)):
            all_runs[key].append(float(t))

    # Build (cat, bench, size) -> {lang -> stats_dict}
    groups: dict = defaultdict(dict)
    for (cat, bench, size, lang), runs in all_runs.items():
        if not runs:
            continue
        n = len(runs)
        mean   = statistics.mean(runs)
        median = statistics.median(runs)
        stdev  = statistics.stdev(runs) if n > 1 else 0.0
        cv     = stdev / mean if mean > 0 else None
        groups[(cat, bench, size)][lang] = {
            "mean":   round(mean,   6),
            "median": round(median, 6),
            "stdev":  round(stdev,  6),
            "cv":     round(cv, 4) if cv is not None else None,
            "min":    round(min(runs), 6),
            "max":    round(max(runs), 6),
            "n":      n,
            "runs":   [round(r, 6) for r in runs],
        }
    return dict(groups)


def compute_speedups(groups: dict) -> list:
    """
    For each (cat, bench, size) group, compute speedup vs C baseline.
    speedup = c_mean / lang_mean  (>1 means lang is faster than C — unlikely
    outside micro-benchmarks, but included for completeness).
    Returns a flat list of record dicts.
    """
    rows = []
    for (cat, bench, size), lang_stats in sorted(groups.items()):
        c_mean = lang_stats.get("c", {}).get("mean")
        for lang, stats in sorted(lang_stats.items()):
            speedup = None
            if c_mean and c_mean > 0 and stats["mean"] > 0:
                # speedup > 1: lang is faster; < 1: lang is slower
                speedup = round(c_mean / stats["mean"], 4)
            rows.append({
                "category":  cat,
                "benchmark": bench,
                "size":      size,
                "language":  lang,
                "speedup_vs_c": speedup,
                **stats,
            })
    return rows


def find_outliers(groups: dict) -> list:
    """
    Within each (cat, bench, size, lang) group, flag individual runs that are
    more than OUTLIER_SIGMA standard deviations above the group mean.
    """
    flagged = []
    for (cat, bench, size), lang_stats in groups.items():
        for lang, stats in lang_stats.items():
            runs = stats["runs"]
            mean  = stats["mean"]
            stdev = stats["stdev"]
            if stdev == 0 or len(runs) < 3:
                continue
            threshold = mean + OUTLIER_SIGMA * stdev
            for i, t in enumerate(runs):
                if t > threshold:
                    flagged.append({
                        "category":   cat,
                        "benchmark":  bench,
                        "size":       size,
                        "language":   lang,
                        "run_index":  i,
                        "run_time_s": round(t, 6),
                        "mean_s":     mean,
                        "stdev_s":    stdev,
                        "sigma":      round((t - mean) / stdev, 2),
                    })
    return sorted(flagged, key=lambda r: -r["sigma"])


def by_category(rows: list) -> dict:
    grouped: dict = defaultdict(list)
    for r in rows:
        grouped[r["category"]].append(r)
    return dict(grouped)


def print_summary(rows: list) -> None:
    current_cat = None
    for r in rows:
        if r["category"] != current_cat:
            current_cat = r["category"]
            print(f"\n── {current_cat} ──")
        speedup_str = f"  speedup={r['speedup_vs_c']:.2f}x" if r["speedup_vs_c"] is not None else ""
        cv_str = f"  CV={r['cv']:.1%}" if r["cv"] is not None else ""
        print(
            f"  {r['benchmark']:20s} {r['language']:8s} {r['size']:6s}"
            f"  mean={r['mean']:.4f}s{speedup_str}{cv_str}"
        )


def main() -> None:
    ap = argparse.ArgumentParser(description="Phase 4 results analyzer")
    ap.add_argument("--category", default=None, help="Analyze only one category")
    ap.add_argument("--print-summary", action="store_true", help="Print text summary")
    args = ap.parse_args()

    records = load_raw_results(args.category)
    if not records:
        print(f"No result files found in {RAW_DIR}")
        print("Run scripts/run_all.sh first to collect benchmark data.")
        sys.exit(0)

    print(f"Loaded {len(records)} result files from {RAW_DIR}\n")

    groups   = aggregate(records)
    rows     = compute_speedups(groups)
    outliers = find_outliers(groups)
    by_cat   = by_category(rows)

    if args.print_summary:
        print("=== Benchmark Summary (mean wall-clock time, speedup vs C) ===")
        print_summary(rows)
        print()

    # ── write processed outputs ───────────────────────────────────────────────
    PROCESSED_DIR.mkdir(parents=True, exist_ok=True)

    summary_path = PROCESSED_DIR / "summary.json"
    with open(summary_path, "w") as f:
        json.dump(rows, f, indent=2)
    print(f"Summary written to:      {summary_path}  ({len(rows)} rows)")

    by_cat_path = PROCESSED_DIR / "by_category.json"
    with open(by_cat_path, "w") as f:
        json.dump(by_cat, f, indent=2)
    print(f"By-category written to:  {by_cat_path}")

    outlier_path = PROCESSED_DIR / "outliers.json"
    with open(outlier_path, "w") as f:
        json.dump(outliers, f, indent=2)
    print(f"Outliers written to:     {outlier_path}  ({len(outliers)} flagged)")

    if outliers:
        print(f"\nTop outliers (>{OUTLIER_SIGMA}σ above mean):")
        for o in outliers[:10]:
            print(
                f"  {o['category']}/{o['benchmark']} {o['language']} {o['size']}"
                f"  run#{o['run_index']}={o['run_time_s']:.4f}s"
                f"  mean={o['mean_s']:.4f}s  {o['sigma']:.1f}σ"
            )


if __name__ == "__main__":
    main()
