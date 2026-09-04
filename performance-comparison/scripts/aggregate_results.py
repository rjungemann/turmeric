#!/usr/bin/env python3
"""
Phase 5: Aggregate and normalize benchmark results.

Reads all JSON files from results/raw/, applies trimmed-mean normalization
per the documented methodology (discard top/bottom 10% of per-run times),
then normalizes each benchmark's mean time to the C baseline (C = 1.0).

Outputs:
  results/processed/normalized.json  -- per-(cat,bench,size,lang) entry with
                                        trimmed_mean_s, speedup_vs_c, and summary stats
  results/processed/rankings.json    -- per-(cat,bench,size) language ranking
                                        by trimmed_mean_s

Usage:
  python3 scripts/aggregate_results.py [--category <cat>] [--size <size>]
"""

import argparse
import json
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path

BASE_DIR      = Path(__file__).resolve().parent.parent
RAW_DIR       = BASE_DIR / "results" / "raw"
PROCESSED_DIR = BASE_DIR / "results" / "processed"

TRIM_FRACTION = 0.10   # discard top and bottom 10% of runs
# B3/B4 (post-jit-benchmark-resurrection-plan): the current list is KEPT
# verbatim and the new columns ride on top -- haskell (B2) and turjit (B4).
LANGUAGES     = ["c", "turmeric", "rust", "clojure", "racket", "turi", "python",
                 "haskell", "turjit"]


def load_raw(cat_filter: "str | None", size_filter: "str | None") -> list:
    records = []
    if not RAW_DIR.exists():
        return records
    for path in sorted(RAW_DIR.glob("*.json")):
        try:
            with open(path) as f:
                data = json.load(f)
        except (json.JSONDecodeError, OSError):
            continue
        if not all(k in data for k in ("language", "category", "benchmark", "size", "timing_s")):
            continue
        if cat_filter  and data["category"] != cat_filter:
            continue
        if size_filter and data["size"] != size_filter:
            continue
        records.append(data)
    return records


def trimmed_stats(runs: list, trim: float = TRIM_FRACTION) -> dict:
    """
    Compute mean/median/stdev after discarding the lowest and highest
    `trim` fraction of values. With <3 values, skip trimming.
    """
    if not runs:
        return {"trimmed_mean": None, "trimmed_median": None,
                "trimmed_stdev": None, "n_raw": 0, "n_trimmed": 0}
    n_raw = len(runs)
    sorted_runs = sorted(runs)
    k = math.floor(n_raw * trim)  # number to cut from each end
    trimmed = sorted_runs[k: n_raw - k] if n_raw >= 3 and k > 0 else sorted_runs
    n_t = len(trimmed)
    mean   = statistics.mean(trimmed)
    median = statistics.median(trimmed)
    stdev  = statistics.stdev(trimmed) if n_t > 1 else 0.0
    cv     = stdev / mean if mean > 0 else None
    return {
        "trimmed_mean":   round(mean,   6),
        "trimmed_median": round(median, 6),
        "trimmed_stdev":  round(stdev,  6),
        "trimmed_cv":     round(cv, 4) if cv is not None else None,
        "n_raw":     n_raw,
        "n_trimmed": n_t,
        "min": round(min(trimmed), 6),
        "max": round(max(trimmed), 6),
    }


def aggregate(records: list) -> dict:
    """Group by (cat, bench, size, lang) -> list of individual run times."""
    all_runs: dict = defaultdict(list)
    for r in records:
        key = (r["category"], r["benchmark"], r["size"], r["language"])
        t = r["timing_s"]
        if isinstance(t, dict) and "runs" in t:
            all_runs[key].extend(t["runs"])
        elif isinstance(t, (int, float)):
            all_runs[key].append(float(t))
    return dict(all_runs)


def build_normalized(all_runs: dict) -> tuple:
    """
    Build normalized entries and language rankings.
    Returns (normalized_list, rankings_dict).
    """
    # First pass: compute trimmed stats per key
    stats_map: dict = {}
    for key, runs in all_runs.items():
        stats_map[key] = trimmed_stats(runs)

    # Group by (cat, bench, size) to find C baseline
    group_keys: dict = defaultdict(list)
    for (cat, bench, size, lang) in stats_map:
        group_keys[(cat, bench, size)].append(lang)

    normalized = []
    rankings   = {}

    for (cat, bench, size), langs in sorted(group_keys.items()):
        c_mean = stats_map.get((cat, bench, size, "c"), {}).get("trimmed_mean")
        # B3 (section 4.1): the normalization anchor is a PARAMETER, default
        # rust -- present in every category, unlike C (two of nine).  The
        # baseline language is recorded in every row so a chart can never be
        # read against the wrong anchor.  speedup_vs_c stays wherever C exists.
        base_mean = stats_map.get((cat, bench, size, BASELINE), {}).get("trimmed_mean")

        rows_for_group = []
        for lang in LANGUAGES:
            key = (cat, bench, size, lang)
            if key not in stats_map:
                continue
            s = stats_map[key]
            speedup = None
            if c_mean and c_mean > 0 and s["trimmed_mean"] and s["trimmed_mean"] > 0:
                speedup = round(c_mean / s["trimmed_mean"], 4)
            speedup_base = None
            if base_mean and base_mean > 0 and s["trimmed_mean"] and s["trimmed_mean"] > 0:
                speedup_base = round(base_mean / s["trimmed_mean"], 4)
            entry = {
                "category":    cat,
                "benchmark":   bench,
                "size":        size,
                "language":    lang,
                "speedup_vs_c": speedup,
                "speedup_vs_baseline": speedup_base,
                "baseline_language": BASELINE,
                **s,
            }
            normalized.append(entry)
            rows_for_group.append((lang, s["trimmed_mean"]))

        # Rank languages fastest -> slowest (None last)
        ranked = sorted(
            rows_for_group,
            key=lambda t: (t[1] is None, t[1] or float("inf")),
        )
        rankings[f"{cat}/{bench}/{size}"] = [
            {"rank": i + 1, "language": lang, "trimmed_mean_s": mean}
            for i, (lang, mean) in enumerate(ranked)
        ]

    return normalized, rankings


BASELINE = "rust"

def main() -> None:
    global BASELINE
    ap = argparse.ArgumentParser(description="Phase 5: aggregate and normalize results")
    ap.add_argument("--category", default=None)
    ap.add_argument("--size",     default=None, choices=["small", "medium", "large"])
    ap.add_argument("--baseline", default="rust", choices=LANGUAGES,
                    help="normalization anchor for speedup_vs_baseline "
                         "(default: rust -- present in every category)")
    ap.add_argument("--trim",     type=float, default=TRIM_FRACTION,
                    help=f"Trim fraction (default {TRIM_FRACTION})")
    args = ap.parse_args()
    BASELINE = args.baseline

    records = load_raw(args.category, args.size)
    if not records:
        print(f"No result files in {RAW_DIR}. Run scripts/run_all.sh first.")
        sys.exit(0)

    print(f"Loaded {len(records)} raw result files.")
    all_runs = aggregate(records)
    normalized, rankings = build_normalized(all_runs)

    PROCESSED_DIR.mkdir(parents=True, exist_ok=True)

    norm_path = PROCESSED_DIR / "normalized.json"
    with open(norm_path, "w") as f:
        json.dump(normalized, f, indent=2)
    print(f"Normalized data  -> {norm_path}  ({len(normalized)} entries)")

    rank_path = PROCESSED_DIR / "rankings.json"
    with open(rank_path, "w") as f:
        json.dump(rankings, f, indent=2)
    print(f"Rankings         -> {rank_path}  ({len(rankings)} groups)")

    # Quick summary table
    print(f"\nTop speedups vs C  (trim={args.trim:.0%}):")
    top = sorted(
        [e for e in normalized if e["speedup_vs_c"] is not None and e["language"] != "c"],
        key=lambda e: -e["speedup_vs_c"],
    )[:15]
    for e in top:
        print(
            f"  {e['category']:20s} {e['benchmark']:20s} {e['language']:8s}"
            f"  {e['trimmed_mean']:.4f}s  speedup={e['speedup_vs_c']:.3f}x"
        )


if __name__ == "__main__":
    main()
