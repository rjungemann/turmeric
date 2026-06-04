# `tur parse-check` flags `: T` (spaced) vs `:T` annotation as an AST mismatch

## Summary

`tur parse-check a.tur b.sweet` reports a mismatch when one side writes a
type annotation as `: int` (colon, space, type -- the documented traditional
style) and the other writes `:int`. The two forms are semantically identical
and both compile, but `parse-check` compares raw reader output and the
traditional reader keeps `:` and `int` as two separate atoms while the
sweet-exp reader (and the unspaced `:int` traditional form) yield a single
`:int` keyword atom.

**Severity:** ergonomics / tooling-gap (not a miscompile). It makes the
`Check guide toggle pairs` CI job (`tools/check-guide-pairs.py`) red:
248 of 826 guide pairs fail purely on this spacing difference.

**Pre-existing on `main`** -- reproduced on a clean `origin/main` build
(826 found / 578 ok / 248 failed, identical to the merge). Not introduced by
the closure-capture work in PR #230.

## Minimal repro

```sh
printf '(defn main [] : int 0)\n'                 > /tmp/t.tur   # spaced ": int"
printf '#lang sweet-exp\ndefn main [] :int\n  0\n' > /tmp/s.sweet # ":int"
tur parse-check /tmp/t.tur /tmp/s.sweet
# tur: parse-check mismatch between /tmp/t.tur and /tmp/s.sweet
# --- /tmp/t.tur ---
# (defn main [] : int 0)        <- ':' and 'int' are two atoms
# --- /tmp/s.sweet ---
# (defn main [] :int 0)         <- ':int' is one keyword atom
```

Control (unspaced traditional matches sweet):

```sh
printf '(defn main [] :int 0)\n' > /tmp/t2.tur
tur parse-check /tmp/t2.tur /tmp/s.sweet   # exit 0, no mismatch
```

## Observed vs expected

- **Observed:** spaced `: int` and unspaced `:int` parse to structurally
  different forms (5-element vs 4-element `defn`), so `parse-check` reports a
  mismatch even though both elaborate identically.
- **Expected:** the type-annotation colon should canonicalize the same way
  regardless of whitespace, so `: int` and `:int` compare equal (matching how
  the elaborator already treats them).

## Root cause (direction)

The traditional reader tokenizes a bare `:` separately from the following
type, whereas `:int` (no space) and the sweet-exp `:` annotation both produce
a single `:int` atom. `parse-check`'s AST comparison is purely syntactic and
does not normalize the annotation colon. The guides legitimately use the
documented spaced style (`[name : cstr] : void`, see `CLAUDE.md` indentation
section) on the traditional side and `:int` on the sweet side.

## Proposed fix directions

1. **Normalize in `parse-check`** (preferred): fold a standalone `:` + type
   into the `:type` annotation atom (or strip annotation-colon whitespace)
   before comparing ASTs, so spacing is not significant. One change, fixes all
   248 pairs, keeps the documented spaced style legal in guides.
2. **Normalize at the reader level**: make the traditional reader emit a single
   `:type` atom for `: type` in annotation position. Broader blast radius;
   needs care around keyword vs annotation contexts.
3. **Edit the guides** to use unspaced `:int` on the traditional side (or
   regenerate via `tools/genguides.py` with consistent spacing). High churn
   (248 pairs) and pushes against the documented spaced style.

## Validation

- `python3 tools/check-guide-pairs.py --tur build/tur docs/guides/` must report
  `Pairs failed : 0`.
- Add a focused `parse-check` fixture asserting `(defn f [] : int 0)` and
  `(defn f [] :int 0)` compare equal.
