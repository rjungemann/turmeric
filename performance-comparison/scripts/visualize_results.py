#!/usr/bin/env python3
"""
Phase 5: Visualize benchmark results.

Reads results/processed/normalized.json (produced by aggregate_results.py)
and generates:
  1. ASCII bar charts printed to stdout (one per category/benchmark).
  2. analysis/report.html -- self-contained HTML report with Chart.js bar charts,
     a speedup heatmap table, and a sortable results table.

Usage:
  python3 scripts/visualize_results.py [--category <cat>] [--no-html] [--no-ascii]
"""

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

BASE_DIR      = Path(__file__).resolve().parent.parent
PROCESSED_DIR = BASE_DIR / "results" / "processed"
ANALYSIS_DIR  = BASE_DIR / "analysis"

LANG_ORDER    = ["c", "turmeric", "rust", "clojure", "racket", "turi", "python"]
LANG_COLORS   = {
    "c":       "#4e9af1",
    "turmeric":"#f0a500",
    "rust":    "#ce422b",
    "clojure": "#63b76c",
    "racket":  "#e05c5c",
    "turi":    "#8c6d00",
    "python":  "#9b59b6",
}
BAR_WIDTH     = 40   # max ASCII bar width in chars


# ── ASCII visualisation ───────────────────────────────────────────────────────

def ascii_bar(value: float, max_value: float, width: int = BAR_WIDTH) -> str:
    if max_value <= 0:
        return ""
    filled = round(value / max_value * width)
    return "█" * filled + "░" * (width - filled)


def print_ascii_charts(entries: list, cat_filter: "str | None") -> None:
    # Group by (category, benchmark, size)
    groups: dict = defaultdict(dict)
    for e in entries:
        if cat_filter and e["category"] != cat_filter:
            continue
        groups[(e["category"], e["benchmark"], e["size"])][e["language"]] = e

    if not groups:
        print("No data to display.")
        return

    for (cat, bench, size), lang_map in sorted(groups.items()):
        print(f"\n── {cat}/{bench}  [{size}] ─────────────────────────────────")
        means = {l: d["trimmed_mean"] for l, d in lang_map.items()
                 if d.get("trimmed_mean") is not None}
        max_mean = max(means.values()) if means else 1.0

        for lang in LANG_ORDER:
            if lang not in lang_map:
                continue
            e = lang_map[lang]
            t = e.get("trimmed_mean")
            if t is None:
                print(f"  {lang:8s}  {'(no data)':>8}")
                continue
            speedup = e.get("speedup_vs_c")
            sp_str  = f"  {speedup:.2f}x vs C" if speedup is not None and lang != "c" else "  baseline"
            bar     = ascii_bar(t, max_mean)
            print(f"  {lang:8s}  {t:8.4f}s  {bar}  {sp_str}")


# ── HTML report ──────────────────────────────────────────────────────────────

def build_chart_datasets(lang_map: dict, labels: list) -> list:
    datasets = []
    for lang in LANG_ORDER:
        data = []
        for lbl in labels:
            e = lang_map.get(lbl, {}).get(lang)
            data.append(round(e["trimmed_mean"], 6) if e and e.get("trimmed_mean") else None)
        datasets.append({
            "label":           lang,
            "data":            data,
            "backgroundColor": LANG_COLORS.get(lang, "#aaa"),
        })
    return datasets


def build_speedup_table_html(entries: list) -> str:
    # speedup_vs_c table:  row=(cat/bench/size), col=language
    rows: dict = defaultdict(dict)
    for e in entries:
        key = f"{e['category']}/{e['benchmark']} [{e['size']}]"
        rows[key][e["language"]] = e.get("speedup_vs_c")

    lines = [
        "<table class='results-table'>",
        "<thead><tr><th>Benchmark</th>"
        + "".join(f"<th>{l}</th>" for l in LANG_ORDER)
        + "</tr></thead><tbody>",
    ]
    for key, lang_vals in sorted(rows.items()):
        cells = []
        for lang in LANG_ORDER:
            sv = lang_vals.get(lang)
            if sv is None:
                cells.append("<td class='no-data'>—</td>")
            elif lang == "c":
                cells.append("<td class='baseline'>1.00×</td>")
            else:
                cls = "faster" if sv >= 0.9 else ("similar" if sv >= 0.5 else "slower")
                cells.append(f"<td class='{cls}'>{sv:.2f}×</td>")
        lines.append(f"<tr><td class='bench-name'>{key}</td>{''.join(cells)}</tr>")
    lines += ["</tbody></table>"]
    return "\n".join(lines)


CHART_JS_CDN = "https://cdn.jsdelivr.net/npm/chart.js@4/dist/chart.umd.min.js"


def build_html(entries: list, cat_filter: "str | None") -> str:
    # Collect all (cat, bench, size) groups
    groups: dict = defaultdict(lambda: defaultdict(dict))
    for e in entries:
        if cat_filter and e["category"] != cat_filter:
            continue
        groups[(e["category"], e["benchmark"])][e["size"]][e["language"]] = e

    if not groups:
        return "<p>No data available. Run scripts/run_all.sh first.</p>"

    charts_js = []
    chart_divs = []
    chart_idx = 0

    for (cat, bench), size_map in sorted(groups.items()):
        for size, lang_map in sorted(size_map.items()):
            cid = f"chart{chart_idx}"
            chart_idx += 1
            langs_present = [l for l in LANG_ORDER if l in lang_map
                             and lang_map[l].get("trimmed_mean") is not None]
            times = [round(lang_map[l]["trimmed_mean"], 6) for l in langs_present]
            colors = [LANG_COLORS.get(l, "#aaa") for l in langs_present]

            chart_divs.append(
                f"<div class='chart-container'>"
                f"<h3>{cat} / {bench} <span class='size-badge'>{size}</span></h3>"
                f"<canvas id='{cid}'></canvas></div>"
            )
            charts_js.append(f"""
  new Chart(document.getElementById('{cid}'), {{
    type: 'bar',
    data: {{
      labels: {json.dumps(langs_present)},
      datasets: [{{
        label: 'Wall-clock time (s)',
        data: {json.dumps(times)},
        backgroundColor: {json.dumps(colors)},
        borderWidth: 1,
      }}]
    }},
    options: {{
      responsive: true,
      plugins: {{ legend: {{ display: false }}, title: {{ display: false }} }},
      scales: {{ y: {{ beginAtZero: true, title: {{ display: true, text: 'seconds' }} }} }},
    }}
  }});""")

    speedup_table = build_speedup_table_html(
        [e for e in entries if not cat_filter or e["category"] == cat_filter]
    )

    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Turmeric Performance Comparison — Results Report</title>
<script src="{CHART_JS_CDN}"></script>
<style>
  body {{ font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
          margin: 0; padding: 2rem; background: #f5f5f7; color: #1d1d1f; }}
  h1   {{ font-size: 1.8rem; margin-bottom: 0.25rem; }}
  h2   {{ font-size: 1.3rem; margin-top: 2.5rem; border-bottom: 2px solid #e0e0e0; padding-bottom: .4rem; }}
  h3   {{ font-size: 1rem; color: #555; margin-bottom: .5rem; }}
  .subtitle {{ color: #666; margin-bottom: 2rem; }}
  .charts-grid {{ display: grid; grid-template-columns: repeat(auto-fill, minmax(380px, 1fr)); gap: 1.5rem; }}
  .chart-container {{ background: #fff; border-radius: 10px; padding: 1.2rem;
                       box-shadow: 0 1px 6px rgba(0,0,0,.08); }}
  .size-badge {{ background: #e8f0fe; color: #1a73e8; font-size: .75rem;
                 padding: 2px 7px; border-radius: 99px; font-weight: 600; }}
  .results-table {{ width: 100%; border-collapse: collapse; font-size: .85rem; }}
  .results-table th, .results-table td {{ padding: .45rem .75rem; border: 1px solid #ddd; text-align: center; }}
  .results-table th {{ background: #f0f0f0; font-weight: 600; }}
  .bench-name {{ text-align: left; font-family: monospace; }}
  .baseline  {{ color: #555; }}
  .faster    {{ background: #d4edda; color: #155724; font-weight: 600; }}
  .similar   {{ background: #fff3cd; color: #856404; }}
  .slower    {{ background: #f8d7da; color: #721c24; }}
  .no-data   {{ color: #aaa; }}
</style>
</head>
<body>
<h1>Turmeric Performance Comparison</h1>
<p class="subtitle">
  C · Turmeric · Rust · Clojure · Racket · Turi · Python &nbsp;|&nbsp;
  Trimmed mean (±10%) &nbsp;|&nbsp; Speedup relative to C baseline
</p>

<h2>Execution Time by Benchmark</h2>
<div class="charts-grid">
{"".join(chart_divs)}
</div>

<h2>Speedup vs C Baseline</h2>
<p style="color:#555;font-size:.9rem">
  Values &gt; 1× mean faster than C; values &lt; 1× mean slower than C.
  Green = within 10% of C; yellow = 0.5–0.9× C; red = &lt; 0.5× C.
</p>
{speedup_table}

<script>
{"".join(charts_js)}
</script>
</body>
</html>"""
    return html


def main() -> None:
    ap = argparse.ArgumentParser(description="Phase 5: visualize results")
    ap.add_argument("--category", default=None)
    ap.add_argument("--no-html",  action="store_true")
    ap.add_argument("--no-ascii", action="store_true")
    args = ap.parse_args()

    norm_path = PROCESSED_DIR / "normalized.json"
    if not norm_path.exists():
        print(f"Normalized data not found at {norm_path}.")
        print("Run scripts/aggregate_results.py first.")
        sys.exit(0)

    with open(norm_path) as f:
        entries = json.load(f)

    if not entries:
        print("normalized.json is empty — no benchmark data to visualize.")
        print("Run scripts/run_all.sh then scripts/aggregate_results.py first.")
        sys.exit(0)

    if not args.no_ascii:
        print("=== ASCII Charts ===")
        print_ascii_charts(entries, args.category)

    if not args.no_html:
        ANALYSIS_DIR.mkdir(parents=True, exist_ok=True)
        html = build_html(entries, args.category)
        html_path = ANALYSIS_DIR / "report.html"
        html_path.write_text(html)
        print(f"\nHTML report written to: {html_path}")


if __name__ == "__main__":
    main()
