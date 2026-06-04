# `tur fmt` does not round-trip recent syntax: drops comments and splits spaced type annotations

## Summary

`tur fmt` fails to round-trip ten hand-authored stdlib files, so the
`fmt-bootstrap-stdlib` invariant in `tests/run-fmt.sh` (FT7: "every
hand-authored stdlib file is already self-formatted") now fails. The
failures group into one **high-severity** defect and several
**medium/low-severity** ones:

1. **Silent comment loss (high).** A `;;` comment that sits *inside* a
   form -- e.g. between a `defn` signature and its body -- is dropped on
   reformat. This is silent source destruction, not a cosmetic diff.
2. **Spaced type annotations are split across lines (medium).** When a
   `defn`/`fn` parameter vector is wider than the line limit, the
   formatter puts a parameter name and its `: type` annotation on
   *separate* lines, directly violating the CLAUDE.md rule "never split a
   name and its value (or a name, type, and value triple) across separate
   lines." The hand-written one-line form is correct; the formatter
   mangles it.
3. **Inline-C first line collapses onto the fence (low).** ` ```c\n<code>`
   is reformatted to ` ```c <code>` on one line.
4. **`(load ...)` / blank-line / comment reordering (low).** Shares root
   cause with (1).

Severity overall: the suite is RED (`tur_fmt_tests` / `fmt-bootstrap-stdlib`
fails), and running `tur fmt -w` over stdlib today would **delete comments**
and produce style-guide-violating parameter lists. Do **not** "fix" the test
by reformatting the files with the buggy formatter.

Affected files (all flagged by `fmt --check`):
`stdlib/sized.tur`, `stdlib/arrow.tur`, `stdlib/nat.tur`,
`stdlib/reactor.tur`, `stdlib/httpd.tur`, `stdlib/threadpool.tur`,
`stdlib/httpd-compress.tur`, `stdlib/hamt.tur`, `stdlib/chan.tur`,
`stdlib/map.tur`.

## Reproduction

Build, then run the formatter check / bootstrap test:

```sh
cmake --build build -j --config Debug
bash tests/run-fmt.sh        # -> FAIL fmt-bootstrap-stdlib: stdlib is not self-formatted: ...
```

### (1) Comment loss -- minimal repro

`stdlib/map.tur`, `map-eq-dynamic` (around line 667):

```sh
diff <(./build/tur fmt --stdout stdlib/map.tur) stdlib/map.tur
```

Observed: the two `;;` lines between the `defn` signature and the inline-C
body vanish from the formatter output:

```turmeric
(defn map-eq-dynamic [m1 : int m2 : int ^fat val-cmp] : bool
  ;; CRU B-3: val-cmp is a fat closure box; threaded as int64 to the runtime,
  ;; which fat-dispatches it (tur_hamt_eq_dynamic reads slot 0).
  ```c return tur_hamt_eq_dynamic(m1, m2, (int64_t)(intptr_t)val_cmp); ```)
```

- **Observed:** the two `;;` comment lines are absent after `tur fmt`.
- **Expected:** comments inside a form body are preserved verbatim.

### (2) Spaced type annotation split -- minimal repro

`stdlib/reactor.tur`, `reactor-add-signal` (around line 316),
`stdlib/hamt.tur` `hamt/reduce` (~398), `stdlib/threadpool.tur`
`thread-pool-submit` (~414):

```sh
diff <(./build/tur fmt --stdout stdlib/hamt.tur) stdlib/hamt.tur
```

- **Observed** (formatter output):

  ```turmeric
  (defn hamt/reduce [m
                     : ptr<void>
                     fn
                     : ptr<void>
                     init
                     : ptr<void>
                     ctx
                     : ptr<void>]
    ...)
  ```

- **Expected** (current, correct source): one parameter triple per line,
  or all on one line if it fits:

  ```turmeric
  (defn hamt/reduce [m : ptr<void> fn : ptr<void> init : ptr<void> ctx : ptr<void>]
    ...)
  ```

## Root-cause analysis

### Comment loss

Comments are recovered only from the *gaps between top-level forms*. The
pretty-printer (`fmt_form`) works from the parsed AST, which carries no
comment nodes; comments are re-scanned out of the source text only in the
inter-form gap:

- `src/compiler/fmt.c:1037-1076` -- the top-level loop calls
  `emit_comments_in_gap(prev_end, f->span.off_start)` between consecutive
  top-level forms and once before the first / after the last form.
- `src/compiler/fmt.c:910` -- `emit_comments_in_gap` only scans the byte
  range it is handed.
- File header comment at `src/compiler/fmt.c:11-13` documents the design:
  "The source text is re-scanned between form spans to extract comments.
  If opts.src is NULL, comments are dropped..."

A comment *inside* a form's span (between a `defn`'s signature and its
body, between body forms, etc.) falls inside `f->span`, never inside an
inter-form gap, so it is never re-emitted -> silent loss. The
`httpd-compress.tur` `(load ...)`/blank-line reordering is the same
limitation surfacing differently.

### Spaced type annotation split

- `src/compiler/fmt.c:668` -- `fmt_vec_broken` emits **one vector element
  per line** with no grouping when a vector exceeds the line width.
- `src/compiler/fmt.c:372-375` -- `fmt_defn` formats the parameter vector
  (header item index 2) via the generic `fmt_form`, which dispatches to
  `fmt_vec_broken` when the vector does not fit.

With spaced type annotations (`name : type`, feature #206), a parameter
and its annotation are distinct vector elements, so `fmt_vec_broken` puts
the name on one line and the `: type` on the next. Contrast
`fmt_vec_let_bindings_broken` (`src/compiler/fmt.c:411`), which *does*
group `let`/`loop` bindings as pairs. There is no analogous
annotation-aware grouping for `defn`/`fn` parameter vectors.

## Proposed fix directions

1. **Comment loss (priority).** Make the formatter comment-aware inside
   forms, not just in top-level gaps. Options:
   - Thread an "emit comments in `[prev_child_end, next_child_off)`"
     pass through the recursive `fmt_form`/`fmt_defn`/`fmt_let`/list
     printers, mirroring the top-level gap logic at every nesting level
     (uses the same `Form.span` offsets already available).
   - Or attach leading/trailing comment spans to `Form` nodes at parse
     time and have the printer emit them.
   - As an interim *safety* measure, have `fmt --check`/`-w` refuse to
     rewrite a form whose span contains a `;` comment the printer cannot
     reproduce, so the tool never silently deletes comments.

2. **Parameter-vector grouping.** Add an annotation-aware broken layout
   for `defn`/`fn` parameter vectors: keep each `name`, `name : type`, or
   `^ann name : type` group on a single line (one parameter per line),
   analogous to `fmt_vec_let_bindings_broken`. Detect the vector is a
   parameter list from `fmt_defn`/`fmt_fn` (it is header item index
   2 / index 1) rather than from the generic vector path.

3. **Inline-C fence (low).** Decide a canonical form (keep ` ```c` on its
   own line) and make the printer emit it; ensure the closing ` ```)` rule
   from CLAUDE.md still holds.

## Validation

- `bash tests/run-fmt.sh` -> `fmt-bootstrap-stdlib` passes (all stdlib
  `fmt --check` clean) and `fmt-idempotence-stdlib` still passes.
- The ctest target `tur_fmt_tests` goes green.
- Add a focused regression fixture: a `defn` with (a) an interior `;;`
  comment and (b) a parameter list with spaced type annotations long
  enough to break; assert `fmt(fmt(x)) == fmt(x)` and that the comment
  survives.

## Note: `tur_tests` ctest "timeout" is unrelated / not a bug

The same ctest run reports `tur_tests (Timeout)`. That is the full
`tests/run.sh` fixture suite, which **passes standalone** (`1348 passed, 0
failed`). It only timed out because it was invoked with an over-tight
per-test cap (`ctest --timeout 120`) while sharing four cores with 44 other
ctest jobs; the suite legitimately needs longer. No codebase defect --
recorded here only so the observation is not mistaken for a second bug.
