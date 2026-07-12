# `tur fmt` joins multi-pair `let`/`loop` bindings onto one line (contradicts the one-pair-per-line house style)

**Severity: LOW (formatter house-style divergence, not a correctness bug -- the
output compiles and is idempotent). It contradicts the documented indentation
rule, so `tur fmt` actively rewrites hand-authored, style-compliant code into a
form the style guide says not to use.**

## Summary

`tur fmt` collapses a `let` (or `loop`) binding vector with **two or more
pairs** onto a single line whenever the whole vector fits inside the 80-column
line-width budget. The [CLAUDE.md indentation
rule](../../CLAUDE.md#binding-vectors----align-bindings-under-each-other) says
the opposite:

> In `let`, `loop`, etc., each binding pair is aligned so the names line up.
> **Keep each binding pair on its own line** -- never split a name and its value
> (or a name, type, and value triple) across separate lines.

So a multi-pair `let` that a developer wrote pair-per-line (per the guide) is
reformatted by `tur fmt` into a single joined line. This is exactly the churn
seen while resolving `fmt-bootstrap-stdlib-drift`: e.g. `stdlib/typeclass-show.tur`
had

```turmeric
  (let [iter (hamt/iter-alloc (set-hamt s))
        body (set-show-loop s iter true "")]
    ...)
```

rewritten by the formatter to

```turmeric
  (let [iter (hamt/iter-alloc (set-hamt s)) body (set-show-loop s iter true "")]
    ...)
```

## Minimal repro

```sh
cat > /tmp/letfmt.tur <<'EOF'
(defn g [s] : int
  (let [iter (alloc (hamt s))
        body (loop s iter true "")]
    (destroy iter)))
EOF
./build/tur fmt --stdout /tmp/letfmt.tur
```

Output (two pairs joined onto one line):

```turmeric
(defn g [s] : int
  (let [iter (alloc (hamt s)) body (loop s iter true "")] (destroy iter)))
```

Expected (each pair on its own line, names/values aligned):

```turmeric
(defn g [s] : int
  (let [iter (alloc (hamt s))
        body (loop s iter true "")]
    (destroy iter)))
```

A single-pair `let` collapsing inline is fine (and matches the sweet-exp
examples in CLAUDE.md, e.g. `(let [w (make-window ...)] ...)`); only **2+**
pairs should force the broken layout.

## Root cause

`fmt_let` in `src/compiler/fmt.c:562-575` emits the binding vector **inline**
whenever it measures within the line-width budget, and only falls back to the
pair-per-line layout (`fmt_vec_let_bindings_broken`) when it *overflows*:

```c
/* src/compiler/fmt.c:565 */
if (bindings->tag == F_VEC) {
    uint32_t w = fmt_measure(bindings);
    if (w != UINT32_MAX && s->col + w <= s->opts.line_width) {
        fmt_emit_inline(s, bindings);          /* <- joins all pairs onto one line */
    } else {
        fmt_vec_let_bindings_broken(s, bindings);
    }
}
```

The gate is purely "does it fit in 80 columns", with no consideration of the
number of pairs. So any multi-pair vector under the budget is joined. The
pair-per-line formatter (`fmt_vec_let_bindings_broken`, `src/compiler/fmt.c:518`)
already exists and does the right thing (aligned columns) -- it is just only
reached on overflow.

## Fix directions

- **Gate the inline path on pair count.** The binding vector's `len` is
  `2 * pairs`. Emit inline only when there is at most one pair (`len <= 2`);
  for two or more pairs always call `fmt_vec_let_bindings_broken`, regardless
  of whether the joined form would fit:

  ```c
  const Form *bindings = f->as.list.items[1];
  if (bindings->tag == F_VEC) {
      uint32_t w = fmt_measure(bindings);
      bool single_pair = bindings->as.list.len <= 2;
      if (single_pair && w != UINT32_MAX && s->col + w <= s->opts.line_width) {
          fmt_emit_inline(s, bindings);
      } else {
          fmt_vec_let_bindings_broken(s, bindings);
      }
  }
  ```

  (An empty binding vector `[]`, `len == 0`, stays inline under `single_pair`.)

- **Regenerate the stdlib in the same PR.** This changes the formatter's
  canonical output, so the `FT7 fmt-bootstrap-stdlib` check (`tests/run-fmt.sh`)
  will flag every stdlib file with a multi-pair `let` until they are
  re-`tur fmt`'d. Per the CLAUDE.md "fixture churn is not a deferral reason"
  rule, regenerate `stdlib/*.tur` alongside the formatter change, not in a
  follow-up. Confirm `FT8 fmt-idempotence-stdlib` still passes (the broken
  layout is already idempotent, so this should hold).

- **Add a regression test** next to `fmt-param-vector-no-split` in
  `tests/run-fmt.sh`: a two-pair `let` that fits in 80 columns must still emit
  the second pair on its own line (assert the output contains a newline between
  the pairs, and that it round-trips).

## Scope

Pre-existing formatter behavior; surfaced while resolving
`fmt-bootstrap-stdlib-drift` (`docs/archive/fmt-bootstrap-stdlib-drift.md`),
where regenerating the four drifted stdlib files joined several multi-pair
`let`s onto single lines. That report took the "regenerate to match the
formatter" path (the tractable fix for the red CI check); this report is the
other half -- teach the formatter the house style so regeneration stops
producing joined bindings. Fixing this will re-touch the same stdlib files (and
any others with multi-pair `let`s), so it is a single coordinated regen.
