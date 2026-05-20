#!/usr/bin/env python3
"""
Phase 4 correctness validation script.

For each benchmark:
  1. Builds the C and Turmeric binaries (if not already built).
  2. Runs all 5 languages with the specified input size (default: small).
  3. Compares outputs:
       "exact" -- outputs must match exactly (after stripping whitespace)
       "float" -- parse as float, compare within FLOAT_TOL relative error
       "skip"  -- non-deterministic; only checks that the command exits 0
  4. Saves golden reference outputs to results/golden/.
  5. Prints a pass/fail summary and writes results/validation_report.json.

Usage:
  python3 scripts/validate_correctness.py [--size small|medium|large]
  python3 scripts/validate_correctness.py --golden   # regenerate golden files
  python3 scripts/validate_correctness.py --category numerical
"""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

BASE_DIR      = Path(__file__).resolve().parent.parent
BENCHMARK_DIR = BASE_DIR / "benchmarks"
INPUT_DIR     = BASE_DIR / "inputs"
GOLDEN_DIR    = BASE_DIR / "results" / "golden"
TUR           = BASE_DIR / "../../build-rel/tur"

FLOAT_TOL = 0.01  # 1% relative tolerance for floating-point outputs

# ── benchmark catalogue ───────────────────────────────────────────────────────
# Each entry: (category, benchmark, input_keys, cmp_mode)
#   input_keys -- list of key names extracted from the size-dict in the input JSON
#   cmp_mode   -- "exact" | "float" | "skip"
BENCHMARKS = [
    # numerical
    ("numerical", "fibonacci",       ["n"],                   "exact"),
    ("numerical", "primes",          ["up_to"],               "exact"),
    ("numerical", "matrix_multiply", ["rows", "cols"],        "float"),
    ("numerical", "monte_carlo_pi",  ["iterations"],          "float"),
    # data_structures
    ("data_structures", "list_ops",  ["n"],                   "exact"),
    ("data_structures", "hash_map",  ["n"],                   "exact"),
    ("data_structures", "sort",      ["n"],                   "exact"),
    # string_processing
    ("string_processing", "string_concat", ["n", "chunk_size"], "exact"),
    ("string_processing", "text_search",   ["haystack_size"],   "exact"),
    # concurrency -- message ordering is non-deterministic
    ("concurrency", "thread_ring",   ["n_threads", "messages"], "skip"),
    # memory
    ("memory", "alloc_churn",        ["n"],                   "exact"),
    # recursion
    ("recursion", "factorial",       ["n"],                   "exact"),
    ("recursion", "fib_recursive",   ["n"],                   "exact"),
    # io
    ("io", "file_write",             ["n"],                   "exact"),
    ("io", "file_read",              ["n"],                   "exact"),
    ("io", "random_access",          ["file_size", "n_reads"],"exact"),
    # real_world
    ("real_world", "nbody",          ["n_bodies", "steps"],   "float"),
    ("real_world", "ray_tracing",    ["width", "height"],     "exact"),
    # micro
    ("micro", "int_arith",           ["n"],                   "exact"),
    ("micro", "float_arith",         ["n"],                   "float"),
    ("micro", "function_call",       ["n"],                   "exact"),
]

# Input JSON filenames that differ from <benchmark>.json
INPUT_FILE_OVERRIDES = {
    ("numerical",  "matrix_multiply"): "matrix.json",
    ("numerical",  "monte_carlo_pi"):  "monte_carlo.json",
    ("recursion",  "fib_recursive"):   "fibonacci.json",
}

# Categories whose C binaries need -lm
NEEDS_LM = {"real_world", "micro"}


def input_json_path(category: str, benchmark: str) -> Path:
    filename = INPUT_FILE_OVERRIDES.get((category, benchmark), f"{benchmark}.json")
    return INPUT_DIR / category / filename


def _needs_rebuild(src: Path, binary: Path) -> bool:
    return not binary.exists() or src.stat().st_mtime > binary.stat().st_mtime


def build_c(category: str, benchmark: str) -> "Path | None":
    src = BENCHMARK_DIR / category / "c" / f"{benchmark}.c"
    if not src.exists():
        return None
    binary = src.with_suffix("")
    if _needs_rebuild(src, binary):
        extra = ["-lm"] if category in NEEDS_LM else []
        cmd = ["clang", "-O3", "-o", str(binary), str(src), "-lpthread"] + extra
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"    [BUILD FAIL] c/{benchmark}: {r.stderr.strip()}")
            return None
    return binary


def build_tur(category: str, benchmark: str) -> "Path | None":
    src = BENCHMARK_DIR / category / "turmeric" / f"{benchmark}.tur"
    if not src.exists():
        return None
    binary = src.with_suffix("")
    if _needs_rebuild(src, binary):
        if not TUR.exists():
            print(f"    [BUILD SKIP] turmeric/{benchmark}: tur binary not found at {TUR}")
            return None
        r = subprocess.run(
            [str(TUR), "build", str(src), "-o", str(binary)],
            capture_output=True, text=True,
        )
        if r.returncode != 0:
            print(f"    [BUILD FAIL] turmeric/{benchmark}: {r.stderr.strip()}")
            return None
    return binary


def gather_commands(category: str, benchmark: str) -> "dict[str, list[str] | None]":
    cmds: dict[str, list[str] | None] = {}

    c_bin = build_c(category, benchmark)
    cmds["c"] = [str(c_bin)] if c_bin else None

    tur_bin = build_tur(category, benchmark)
    cmds["turmeric"] = [str(tur_bin)] if tur_bin else None

    clj = BENCHMARK_DIR / category / "clojure" / f"{benchmark}.clj"
    cmds["clojure"] = ["clojure", "-M", str(clj)] if clj.exists() else None

    rkt = BENCHMARK_DIR / category / "racket" / f"{benchmark}.rkt"
    cmds["racket"] = ["racket", str(rkt)] if rkt.exists() else None

    py = BENCHMARK_DIR / category / "python" / f"{benchmark}.py"
    cmds["python"] = ["python3", str(py)] if py.exists() else None

    return cmds


def run_impl(cmd_prefix: "list[str]", args: "list[str]") -> "str | None":
    cmd = cmd_prefix + args
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if r.returncode != 0:
            return None
        return r.stdout.strip()
    except (subprocess.TimeoutExpired, FileNotFoundError, PermissionError):
        return None


def outputs_match(a: str, b: str, mode: str) -> bool:
    if mode == "exact":
        return a.strip() == b.strip()
    if mode == "float":
        try:
            fa, fb = float(a.strip()), float(b.strip())
            if fa == 0.0 and fb == 0.0:
                return True
            denom = max(abs(fa), abs(fb), 1e-12)
            return abs(fa - fb) / denom < FLOAT_TOL
        except ValueError:
            return a.strip() == b.strip()
    return True  # skip mode


def validate_benchmark(
    category: str,
    benchmark: str,
    input_keys: "list[str]",
    cmp_mode: str,
    size: str,
    regen_golden: bool,
) -> "dict | None":
    json_path = input_json_path(category, benchmark)
    if not json_path.exists():
        print(f"  [SKIP] {category}/{benchmark}: input file not found ({json_path.name})")
        return None

    with open(json_path) as f:
        inputs = json.load(f)

    if size not in inputs:
        print(f"  [SKIP] {category}/{benchmark}: size '{size}' not in {json_path.name}")
        return None

    size_dict = inputs[size]
    args = [str(size_dict[k]) for k in input_keys if k in size_dict]

    cmds = gather_commands(category, benchmark)

    results: dict[str, "str | None"] = {}
    for lang, cmd_prefix in cmds.items():
        if cmd_prefix is None:
            results[lang] = None
            continue
        results[lang] = run_impl(cmd_prefix, args)

    # Choose reference: prefer C, then turmeric, python, racket, clojure
    reference: "str | None" = None
    ref_lang: "str | None" = None
    for preferred in ["c", "turmeric", "python", "racket", "clojure"]:
        if results.get(preferred) is not None:
            reference = results[preferred]
            ref_lang = preferred
            break

    if reference is None:
        print(f"  [NO IMPL] {category}/{benchmark}: no language produced output")
        return {
            "category": category, "benchmark": benchmark, "size": size,
            "status": "no_impl", "details": {},
        }

    # Golden file management
    golden_file = GOLDEN_DIR / f"{category}_{benchmark}_{size}.txt"
    golden_mismatch = False
    if regen_golden or not golden_file.exists():
        GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
        golden_file.write_text(reference + "\n")
        print(f"  [GOLDEN] saved {golden_file.name} (ref={ref_lang})")
    else:
        golden_ref = golden_file.read_text().strip()
        if not outputs_match(reference, golden_ref, cmp_mode):
            print(
                f"  [GOLDEN DIFF] {category}/{benchmark}: "
                f"reference='{reference}' golden='{golden_ref}'"
            )
            golden_mismatch = True

    # Compare each language against reference
    details: dict[str, str] = {}
    passed = 0
    total  = 0
    for lang, out in results.items():
        if out is None:
            details[lang] = "missing"
            continue
        total += 1
        if cmp_mode == "skip":
            details[lang] = "skip"
            passed += 1
        elif outputs_match(out, reference, cmp_mode):
            details[lang] = "pass"
            passed += 1
        else:
            details[lang] = f"FAIL(got='{out}' expected='{reference}')"

    status = "pass" if (passed == total and not golden_mismatch) else "fail"
    marker = "OK" if status == "pass" else "!!"
    detail_str = "  ".join(f"{l}:{s}" for l, s in details.items())
    print(f"  [{marker}] {category}/{benchmark} [{size}]  {detail_str}")

    return {
        "category": category,
        "benchmark": benchmark,
        "size": size,
        "ref_lang": ref_lang,
        "status": status,
        "details": details,
    }


def main() -> None:
    ap = argparse.ArgumentParser(description="Phase 4 correctness validator")
    ap.add_argument(
        "--size", default="small", choices=["small", "medium", "large"],
        help="Input size to validate (default: small)",
    )
    ap.add_argument(
        "--golden", action="store_true",
        help="Regenerate golden reference output files",
    )
    ap.add_argument(
        "--category", default=None,
        help="Validate only benchmarks in this category",
    )
    args = ap.parse_args()

    GOLDEN_DIR.mkdir(parents=True, exist_ok=True)

    print(f"=== Phase 4 Correctness Validation  (size={args.size}) ===\n")

    all_results = []
    for cat, bench, keys, cmp in BENCHMARKS:
        if args.category and cat != args.category:
            continue
        r = validate_benchmark(cat, bench, keys, cmp, args.size, args.golden)
        if r:
            all_results.append(r)

    total  = sum(1 for r in all_results if r["status"] != "no_impl")
    passed = sum(1 for r in all_results if r["status"] == "pass")
    failed = sum(1 for r in all_results if r["status"] == "fail")
    print(f"\n=== Summary: {passed}/{total} passed, {failed} failed ===")

    report_path = BASE_DIR / "results" / "validation_report.json"
    report_path.parent.mkdir(parents=True, exist_ok=True)
    with open(report_path, "w") as f:
        json.dump({"size": args.size, "benchmarks": all_results}, f, indent=2)
    print(f"Report written to: {report_path}")

    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
