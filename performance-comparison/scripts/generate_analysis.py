#!/usr/bin/env python3
"""
Phase 5: Generate markdown analysis documents from processed benchmark data.

Reads results/processed/normalized.json and results/processed/rankings.json
(both produced by aggregate_results.py) and writes:

  analysis/comparison.md   -- full cross-language comparison table
  analysis/by_category.md  -- per-category breakdown with rankings
  analysis/by_language.md  -- per-language profile across all benchmarks
  analysis/conclusions.md  -- key findings, trends, and recommendations

Run after aggregate_results.py:
  python3 scripts/aggregate_results.py
  python3 scripts/generate_analysis.py

When no processed data exists, writes template/placeholder documents so the
analysis/ directory is populated and ready for manual editing.
"""

import json
import sys
from collections import defaultdict
from datetime import date
from pathlib import Path

BASE_DIR      = Path(__file__).resolve().parent.parent
PROCESSED_DIR = BASE_DIR / "results" / "processed"
ANALYSIS_DIR  = BASE_DIR / "analysis"

LANG_ORDER = ["c", "turmeric", "clojure", "racket", "python"]
TODAY      = date.today().isoformat()

CATEGORY_DESCRIPTIONS = {
    "numerical":        "Arithmetic-intensive tasks: Fibonacci, prime sieve, matrix multiply, Monte Carlo π.",
    "data_structures":  "Memory and access patterns: list operations, hash map, in-place sort.",
    "string_processing":"Text manipulation: string concatenation, substring search.",
    "concurrency":      "Multi-threading: thread-ring message passing.",
    "memory":           "Allocation and GC pressure: alloc-churn N objects in a loop.",
    "recursion":        "Deep/tail recursion: recursive Fibonacci, factorial.",
    "io":               "File I/O throughput: sequential write, sequential read, random seeks.",
    "real_world":       "Complex algorithms: N-body simulation, ray tracing.",
    "micro":            "Micro-benchmarks: integer arithmetic, floating-point arithmetic, function-call overhead.",
}

LANGUAGE_NOTES = {
    "c":       "Compiled with `clang -O3 -lpthread -lm`. Serves as the baseline (1.0×).",
    "turmeric":"Custom compiled language. Builds via `build-rel/tur build`. Inline-C used for I/O and math-heavy benchmarks.",
    "clojure": "Runs on the JVM (Clojure CLI). JVM startup included in all timings; JIT warmup may benefit repeated runs.",
    "racket":  "Runs on Chez Scheme backend (`racket`). Includes startup overhead.",
    "python":  "CPython 3.13.1 (no JIT). Interpreted; expected to be slowest except for I/O-bound tasks.",
}


# ── helpers ───────────────────────────────────────────────────────────────────

def load_json(path: Path) -> "list | dict | None":
    if not path.exists():
        return None
    try:
        with open(path) as f:
            return json.load(f)
    except (json.JSONDecodeError, OSError):
        return None


def fmt_time(t: "float | None") -> str:
    if t is None:
        return "—"
    if t < 0.001:
        return f"{t * 1000:.3f} ms"
    return f"{t:.4f} s"


def fmt_speedup(s: "float | None", lang: str) -> str:
    if lang == "c":
        return "1.00×"
    if s is None:
        return "—"
    return f"{s:.2f}×"


def md_table(headers: list, rows: list) -> str:
    col_widths = [len(h) for h in headers]
    for row in rows:
        for i, cell in enumerate(row):
            col_widths[i] = max(col_widths[i], len(str(cell)))
    sep  = "| " + " | ".join("-" * w for w in col_widths) + " |"
    head = "| " + " | ".join(str(h).ljust(w) for h, w in zip(headers, col_widths)) + " |"
    body = "\n".join(
        "| " + " | ".join(str(c).ljust(w) for c, w in zip(row, col_widths)) + " |"
        for row in rows
    )
    return f"{head}\n{sep}\n{body}"


def placeholder_note() -> str:
    return (
        "> **Note**: No benchmark results are available yet. "
        "Run `scripts/run_all.sh` followed by `scripts/aggregate_results.py` "
        "and then re-run this script to populate the tables below with real data.\n"
    )


# ── document generators ───────────────────────────────────────────────────────

def gen_comparison(entries: "list | None", rankings: "dict | None") -> str:
    lines = [
        "# Cross-Language Performance Comparison",
        "",
        f"*Generated: {TODAY}*",
        "",
        "Speedup values are relative to the C baseline (C = 1.0×).  "
        "Values > 1× indicate the language ran **faster** than C on that benchmark; "
        "values < 1× indicate it ran slower.",
        "",
    ]

    if not entries:
        lines.append(placeholder_note())
        lines += [
            "## Summary Table",
            "",
            "_(no data — populate by running the benchmark suite)_",
            "",
        ]
        return "\n".join(lines)

    # Index by (cat, bench, size) -> {lang -> entry}
    groups: dict = defaultdict(dict)
    for e in entries:
        groups[(e["category"], e["benchmark"], e["size"])][e["language"]] = e

    headers = ["Category", "Benchmark", "Size"] + [l.capitalize() for l in LANG_ORDER]

    rows = []
    for (cat, bench, size), lang_map in sorted(groups.items()):
        row = [cat, bench, size]
        for lang in LANG_ORDER:
            e = lang_map.get(lang)
            row.append(fmt_speedup(e.get("speedup_vs_c") if e else None, lang))
        rows.append(row)

    lines += ["## Summary Table", "", md_table(headers, rows), ""]
    return "\n".join(lines)


def gen_by_category(entries: "list | None") -> str:
    lines = [
        "# Performance by Category",
        "",
        f"*Generated: {TODAY}*",
        "",
    ]

    if not entries:
        lines.append(placeholder_note())
        for cat, desc in CATEGORY_DESCRIPTIONS.items():
            lines += [f"## {cat.replace('_', ' ').title()}", "", desc, "", "_(no data)_", ""]
        return "\n".join(lines)

    # Group by category
    by_cat: dict = defaultdict(lambda: defaultdict(dict))
    for e in entries:
        by_cat[e["category"]][(e["benchmark"], e["size"])][e["language"]] = e

    for cat in sorted(by_cat):
        desc = CATEGORY_DESCRIPTIONS.get(cat, "")
        lines += [f"## {cat.replace('_', ' ').title()}", "", desc, ""]

        for (bench, size), lang_map in sorted(by_cat[cat].items()):
            lines.append(f"### {bench}  `[{size}]`")
            lines.append("")
            headers = ["Language", "Trimmed Mean", "Stdev", "CV", "Speedup vs C"]
            rows = []
            for lang in LANG_ORDER:
                e = lang_map.get(lang)
                if e is None:
                    continue
                rows.append([
                    lang,
                    fmt_time(e.get("trimmed_mean")),
                    fmt_time(e.get("trimmed_stdev")),
                    f"{e['trimmed_cv']:.1%}" if e.get("trimmed_cv") is not None else "—",
                    fmt_speedup(e.get("speedup_vs_c"), lang),
                ])
            lines += [md_table(headers, rows), ""]

    return "\n".join(lines)


def gen_by_language(entries: "list | None") -> str:
    lines = [
        "# Performance by Language",
        "",
        f"*Generated: {TODAY}*",
        "",
        "Each language's mean execution time across all benchmarks at `small` input size.",
        "",
    ]

    if not entries:
        lines.append(placeholder_note())
        for lang in LANG_ORDER:
            note = LANGUAGE_NOTES.get(lang, "")
            lines += [f"## {lang.capitalize()}", "", note, "", "_(no data)_", ""]
        return "\n".join(lines)

    small_entries = [e for e in entries if e.get("size") == "small"]

    for lang in LANG_ORDER:
        note = LANGUAGE_NOTES.get(lang, "")
        lines += [f"## {lang.capitalize()}", "", note, ""]

        lang_entries = sorted(
            [e for e in small_entries if e["language"] == lang],
            key=lambda e: (e["category"], e["benchmark"]),
        )
        if not lang_entries:
            lines += ["_(no data for this language)_", ""]
            continue

        headers = ["Category", "Benchmark", "Trimmed Mean", "Speedup vs C"]
        rows = []
        for e in lang_entries:
            rows.append([
                e["category"],
                e["benchmark"],
                fmt_time(e.get("trimmed_mean")),
                fmt_speedup(e.get("speedup_vs_c"), lang),
            ])

        # Geometric mean of speedups (excluding missing)
        speedups = [e["speedup_vs_c"] for e in lang_entries
                    if e.get("speedup_vs_c") is not None and lang != "c"]
        if speedups:
            import math
            geo_mean = math.exp(sum(math.log(s) for s in speedups) / len(speedups))
            lines.append(f"*Geometric mean speedup vs C (small): **{geo_mean:.2f}×***")
            lines.append("")

        lines += [md_table(headers, rows), ""]

    return "\n".join(lines)


def gen_conclusions(entries: "list | None", rankings: "dict | None") -> str:
    lines = [
        "# Key Findings and Conclusions",
        "",
        f"*Generated: {TODAY}*",
        "",
        "## Overview",
        "",
        "This document summarises the performance comparison between C, Turmeric, "
        "Clojure, Racket, and Python across nine benchmark categories.",
        "",
    ]

    if not entries:
        lines.append(placeholder_note())
        lines += [
            "## Methodology Recap",
            "",
            "- **Warm-up**: 3 discarded iterations before measurement.",
            "- **Measurement**: 10 timed iterations per benchmark run.",
            "- **Trimming**: Top and bottom 10% of run times discarded before computing the mean.",
            "- **Baseline**: All speedup values are relative to the C `clang -O3` build.",
            "- **Validation**: Outputs cross-checked across languages before timing.",
            "",
            "## Expected Characteristics",
            "",
            "Based on general language properties, we expect:",
            "",
            "| Language | Expected Profile |",
            "|----------|-----------------|",
            "| C        | Fastest or near-fastest in all CPU-bound categories. |",
            "| Turmeric | Close to C for numerical/compiled code; overhead from runtime for dynamic features. |",
            "| Clojure  | JVM startup cost visible at small sizes; JIT benefits at medium/large. |",
            "| Racket   | Competitive with Clojure; Chez backend provides good compiled performance. |",
            "| Python   | 10–100× slower than C on CPU-bound tasks; competitive on I/O-bound tasks. |",
            "",
            "## Limitations",
            "",
            "See [limitations.md](limitations.md) for a full discussion of caveats.",
        ]
        return "\n".join(lines)

    # Build findings from real data
    small = [e for e in entries if e.get("size") == "small"]

    # Turmeric profile
    tur_entries = [e for e in small if e["language"] == "turmeric"
                   and e.get("speedup_vs_c") is not None]
    if tur_entries:
        import math
        geo = math.exp(sum(math.log(e["speedup_vs_c"]) for e in tur_entries) / len(tur_entries))
        best = max(tur_entries, key=lambda e: e["speedup_vs_c"])
        worst = min(tur_entries, key=lambda e: e["speedup_vs_c"])
        lines += [
            "## Turmeric Summary",
            "",
            f"- Geometric mean speedup vs C across all small benchmarks: **{geo:.2f}×**",
            f"- Best result: `{best['category']}/{best['benchmark']}` — "
            f"{best['speedup_vs_c']:.2f}× ({fmt_time(best.get('trimmed_mean'))})",
            f"- Worst result: `{worst['category']}/{worst['benchmark']}` — "
            f"{worst['speedup_vs_c']:.2f}× ({fmt_time(worst.get('trimmed_mean'))})",
            "",
        ]

    # Category winners
    lines += ["## Category Winners (small inputs)", ""]
    headers = ["Category / Benchmark", "Fastest Language", "Time", "vs C"]
    rows = []
    groups: dict = defaultdict(dict)
    for e in small:
        groups[(e["category"], e["benchmark"])][e["language"]] = e
    for (cat, bench), lang_map in sorted(groups.items()):
        valid = [(l, d) for l, d in lang_map.items() if d.get("trimmed_mean") is not None]
        if not valid:
            continue
        fastest_lang, fastest_entry = min(valid, key=lambda t: t[1]["trimmed_mean"])
        rows.append([
            f"{cat}/{bench}",
            fastest_lang,
            fmt_time(fastest_entry.get("trimmed_mean")),
            fmt_speedup(fastest_entry.get("speedup_vs_c"), fastest_lang),
        ])
    lines += [md_table(headers, rows), ""]

    lines += [
        "## Methodology Recap",
        "",
        "- **Warm-up**: 3 discarded iterations before measurement.",
        "- **Measurement**: 10 timed iterations per benchmark run.",
        "- **Trimming**: Top and bottom 10% of run times discarded before computing the mean.",
        "- **Baseline**: All speedup values are relative to the C `clang -O3` build.",
        "- **Validation**: Outputs cross-checked across languages before timing.",
        "",
        "## Limitations",
        "",
        "See [limitations.md](limitations.md) for a full discussion of caveats.",
    ]

    return "\n".join(lines)


def main() -> None:
    ANALYSIS_DIR.mkdir(parents=True, exist_ok=True)

    entries  = load_json(PROCESSED_DIR / "normalized.json")
    rankings = load_json(PROCESSED_DIR / "rankings.json")

    if not entries:
        print("No processed data found — writing placeholder documents.")
        print("Run scripts/run_all.sh + scripts/aggregate_results.py first for real data.")

    docs = {
        "comparison.md":  gen_comparison(entries, rankings),
        "by_category.md": gen_by_category(entries),
        "by_language.md": gen_by_language(entries),
        "conclusions.md": gen_conclusions(entries, rankings),
    }

    for name, content in docs.items():
        path = ANALYSIS_DIR / name
        path.write_text(content)
        print(f"Wrote {path}")


if __name__ == "__main__":
    main()
