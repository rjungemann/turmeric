# `tur fmt --check` fails on committed stdlib files (fmt-bootstrap-stdlib drift)

**RESOLVED (2026-07-12):** Regenerated the four drifted files in place with
`tur fmt` (the preferred one-shot fix). Changes are whitespace-only (each file
verified token-identical to HEAD after stripping whitespace), the formatter is
idempotent on all four, and the full `tests/run-fmt.sh` harness now reports
`17 passed, 0 failed` (FT7 fmt-bootstrap-stdlib and FT8 idempotence both green).
A sweep of every `stdlib/*.tur` (minus `docstrings.tur`, matching FT7's scan)
confirms none remain dirty. No formatter/heuristic changes; layout only.


**Severity: LOW (CI red, no runtime impact; the committed stdlib is valid and
compiles -- it is only out of sync with the current formatter's canonical
output).**

## Summary

The `tur_fmt_tests` CI target (the `FT7 fmt-bootstrap-stdlib` check in
`tests/run-fmt.sh`) runs `tur fmt --check` over a set of stdlib files and fails:
four of them are not in the formatter's current canonical form.

```
stdlib/set.tur            : tur fmt --check -> exit 1
stdlib/vec.tur            : tur fmt --check -> exit 1
stdlib/typeclass.tur      : tur fmt --check -> exit 1
stdlib/typeclass-show.tur : tur fmt --check -> exit 1
```

## Minimal repro

```sh
./build/tur fmt --check stdlib/set.tur ; echo $?    # -> 1
./build/tur fmt --stdout stdlib/set.tur | diff stdlib/set.tur -
```

## Root cause

Pure **drift**: the committed files were formatted by an older formatter (or
hand-edited), and the current formatter emits a different -- but stable --
canonical form. The divergences run in *both* directions (some committed
multi-arg calls the formatter wants joined onto one line, others it wants
broken across lines; plus a handful of blank-line insert/remove tweaks), which
is the signature of formatter output evolving after the files were last
regenerated, not a formatter bug in the files. Confirmed the current formatter
is **idempotent** (`fmt(fmt(x)) == fmt(x)` on both `set.tur` and `vec.tur`), so
the canonical target is stable -- regenerating once fixes it and stays fixed.

Representative diffs (committed -> formatter output):

- `stdlib/set.tur:129` -- `(set-add-eq-o ~s ~h (mk-box ...) (mk-cmp ...) (mk-owned? ...))`
  one-liner -> the formatter breaks each arg onto its own line (call exceeds the
  width budget).
- `stdlib/vec.tur:245` -- committed breaks `(defn vec-empty-like__ ... (:: (vec-new) (Vec A)))`
  across two lines; the formatter joins it onto one.
- Blank-line churn: `stdlib/set.tur:29a30`, `:200d212`, `:543d554`;
  `stdlib/typeclass.tur:21a22`.

## Fix directions

- **Regenerate the four files** in place and commit them:
  ```sh
  for f in stdlib/set.tur stdlib/vec.tur stdlib/typeclass.tur stdlib/typeclass-show.tur; do
    ./build/tur fmt "$f"
  done
  ```
  Since the formatter is idempotent this is a one-shot fix with no behavior
  change (formatting only). Worth a quick scan of the diff first to confirm the
  formatter's line-wrap decisions read acceptably before committing.
- **Alternative (if the committed layout is preferred):** treat this as a
  formatter regression and adjust the width/blank-line heuristics so
  `tur fmt --check` accepts the committed forms -- larger, and only worth it if
  the current output is judged worse than what shipped.

## Scope

Pre-existing and independent of the emit_cps.c removal work (this branch touched
neither the formatter source nor these stdlib files; the divergence is present
at the branch's merge-base with `main`). The formatter and stdlib simply drifted
apart upstream. Surfaced while triaging CI failures on the CPS-lowering-removal
branch.
