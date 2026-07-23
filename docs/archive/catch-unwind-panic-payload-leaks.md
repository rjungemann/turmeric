---
status: resolved
severity: low
discovered: 2026-07-23
resolved: 2026-07-23
area: compiled backend / runtime (catch-unwind, panic payloads, result boxes)
---

# `catch-unwind` / `catch-panic-of` leak the caught result box and panic-message string

## Resolution (2026-07-23)

All three leaks are fixed and every affected fixture now runs clean under
`ASAN_OPTIONS=detect_leaks=1` with ASan forced in (recipe below); their
`requires.no-leak-check` markers are deleted. `bash tests/run.sh` stays green
and no `Invalid free` / use-after-free appears.

- **Leak 1 (panic-message string)** -- `tur_panic_payload` gained an explicit
  `int owns_value` bit (`emit_module.c`). It is set to `1` only on the
  `tur_panic` `strdup(msg)` path and `0` on every `tur_panic_with` path
  (caller-supplied / borrowed / inline-scalar value). `panic_payload_free`
  now frees `payload->value` iff `owns_value`, and `tur_result_box_free`
  routes its err-payload free through `panic_payload_free`, so a caught box's
  owned message is reclaimed when the box is freed -- with no nonheap-free /
  double-free.
- **Leak 2 (caught box read via an opaque reader)** -- a let-bound caught box
  read through a reader that hands back a pointer INTO the box-owned message
  (the fixture's inline-C `panic-msg`) is now deep-freed at scope exit when the
  reader's result is *confined* to the scope (consumed by a non-retaining print
  sink) and the scope value itself cannot carry a box-owned pointer out. See
  `box_uses_confined` / `catch_box_binding_reader_confined` (`emit_core.c`),
  wired into `let_binding_box_freeable` (`emit_expr.c`). The confinement walk
  only ever greenlights a free, so it never frees a still-live box.
- **Leak 3 (stackless aggregate/result box)** -- the stackless segment splitter
  now reclaims a let-bound caught box in its resume segment when the
  continuation is straight-line and non-escaping, via a `free_box` field on the
  terminal `GsSink` emitted just before the delivery break (`emit_fns.c`). This
  removes the `stackless-catch-unwind-result` loop leak (~4.8 MB over 199999
  iters) and, as a bonus, `stackless-catch-unwind-okval` and
  `stackless-catch-unwind-panic-unwind-aggregate-leak`.
- The `catch-panic-of` type-mismatch re-raise path now returns a NULL box
  instead of allocating `tur_box_err(0)` (which the re-raise stranded), fixing
  a 24 B leak in `panic-catch-panic-of` / `panic-with-catch-of`.

Panic-preamble snapshots were regenerated in the same change.

---


## THIS IS NOT CLOSURE DROP-GLUE WORK. Read this line before touching anything.

The closure drop-glue subsystem is **graduated and done** (2026-07-22). The
`catch-unwind` thunk fat-closure env is already dropped at every catch site via
`TUR_CLOSURE_DROP` -- that half is finished and is **not** what this report is
about. This report is a **separate, self-contained** leak in the *result-box /
panic-payload* lifetime, with its own fix that touches the panic runtime
preamble and the caught-Result scope-exit free. It is **not blocked by**, **does
not block**, and is **not part of** closure drop-glue. If your instinct is to
close this as "actually drop-glue / blocked on drop-glue," that instinct is
wrong -- these are heap `tur_result_box_t` / `tur_panic_payload` records with no
drop-glue header, not closures. Fix them here, on their own track.

## Summary

A `catch-unwind` / `catch-panic-of` that catches a panic leaks, per catch site:

1. **The caught panic-message string** -- `(panic "msg")` `strdup`s its message
   into the payload; that string is never freed.
2. **The result box** (24-byte `tur_result_box_t`) -- freed at let-scope exit
   only when the caught Result is inspected *solely* through `ok?`/`err?`/
   `ok-val`. A box read via `err-val`, or handed to any call the escape analysis
   can't see through (e.g. an inline-C accessor), is conservatively left to leak.

Bounded per catch site, so a catch in a loop leaks unboundedly
(`stackless-catch-unwind-result` loses ~4.8 MB over 199999 iterations).

## Affected fixtures (all currently carry `requires.no-leak-check`)

`panic-catch-panic-of`, `panic-catch-unwind-caught`, `panic-catch-unwind-defer`,
`panic-catch-unwind-nested`, `panic-in-handler`, `panic-reset-clears`,
`panic-with-catch-of`, `stackless-catch-unwind-result`.
(`panic-catch-unwind-double` aborts before exit; `panic-catch-unwind-nested-deep`
is already clean.)

Closing this report = these programs are leak-clean under LSan **and** their
`requires.no-leak-check` markers are deleted.

## Minimal repro

```turmeric
(defn boom [] : int (panic "boom happened"))
(defn main [] : int
  (let [r (catch-unwind (fn [] : int (boom)))]
    (if (err? r) (println "caught") (println "no panic")))  ; err? alone: box freed, message still leaks
  0)
```

The suite does **not** sanitize these fixtures by default (they inline
`hamt.c` instead of linking a sanitized `-lturi`, so `tur build`'s ASan
autodetect at `src/main.c:2106` never fires). Reproduce by forcing ASan in:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j
export TUR_CC_FLAGS="-O1 -g -std=c99 -fno-strict-aliasing -fsanitize=address,undefined -fno-omit-frame-pointer -L$(pwd)/build/src"
CC=cc ./build/tur build tests/fixtures/panic-catch-unwind-caught/input.tur -o /tmp/x
ASAN_OPTIONS=detect_leaks=1 /tmp/x    # LeakSanitizer: detected memory leaks
```

## Root cause

### Leak 1 -- panic-message string

`tur_panic` boxes its message as `panic_payload_new(TY_CSTR, strdup(msg), ...)`
(emitted at `src/compiler/emit_module.c:7208`). `tur_result_box_free`
(emitted at `src/compiler/emit_module.c:7403`) frees the payload *record* but
deliberately never frees `payload->value`, because a `(panic-with "literal")`
also produces a `TY_CSTR` payload whose `value` is a **non-heap** string
literal -- freeing it would be a nonheap-free/double-free. The two are
indistinguishable by `type_tag` alone, so the heap `strdup` from `panic` is left
to leak to stay sound. (Same root ambiguity as
`docs/archive/history/panic-with-scalar-payload-free-nonheap.md`.)

### Leak 2 -- caught result box

`let_binding_box_freeable` / `catch_box_binding_escapes`
(`src/compiler/emit_expr.c:1408`) frees a let-bound caught Result at scope exit
only when every use is a read-only `ok?`/`err?`/`ok-val`. A read through
`err-val` (returns the box-owned payload pointer -> freeing the box would
dangle it) or through any opaque call is treated as an escape, and the box is
not freed. `panic-catch-unwind-caught`'s `panic-msg` reads `r->err_val` via
inline-C, so its box is conservatively leaked.

### Leak 3 -- stackless lowering aggregate box

The stackless `catch-unwind` segment splitter keeps its own residual aggregate
box (`tur_box_ok`/`tur_box_err` in the split lowering, `src/compiler/emit_fns.c`
around 1826-1886). This is the `stackless-catch-unwind-result` loop leak.

## Fix directions

1. **Message string (Leak 1).** Give `tur_panic_payload` an explicit ownership
   bit -- e.g. `int owns_value` -- set **true only** on the `tur_panic`
   `strdup(msg)` path (`emit_module.c:7208`) and **false** on every
   `tur_panic_with` path (`emit_module.c:7613,7620`), since those carry a
   caller-supplied/borrowed/scalar value. Then free `payload->value` in
   `panic_payload_free` and `tur_result_box_free` **iff** `owns_value`. This is
   sound: the ambiguity that forced the conservative refusal is exactly what the
   flag resolves. Regenerate the panic-preamble snapshots in the same commit.

2. **Caught box read via `err-val`/opaque call (Leak 2).** Once the payload
   carries `owns_value` and the box's deep free reclaims an owned message, decide
   the box's fate at genuine last-use rather than gating on `err-val`: free the
   box after the last read completes (the extracted `cstr`/scalar has already
   been consumed by then), or copy the extracted value out so the box can be
   freed unconditionally at scope exit. Keep the escape check for stores/returns
   that truly retain the box.

3. **Stackless aggregate box (Leak 3).** Reclaim the split-lowering box on the
   resume path the same way the native path now does; cross-check against
   `docs/archive/history/catch-unwind-aggregate-followups-plan.md` (Part B).

## Done when

Each affected fixture runs clean under `ASAN_OPTIONS=detect_leaks=1` with ASan
forced in (recipe above), its `requires.no-leak-check` marker is deleted, and
no new `Invalid free` / use-after-free appears (used caught Results still return
correct `ok-val`/`err-val`). Regenerate any `expected.c` snapshots that move.
