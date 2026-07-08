---
status: resolved
severity: low
discovered: 2026-07-08
discovered-by: catch-unwind-returned-err-box-payload-leak fix (valgrind repro build)
resolved: 2026-07-08
area: compiled backend / runtime (panic lowering, tur_panic_with abort paths, payload lifetime)
---

## Resolution (2026-07-08)

Fixed in `src/compiler/emit_module.c`. Three opaque-value frees removed, so no
emitted `free()` ever targets the opaque panic value:

- **Both `tur_panic_with` abort paths** -- dropped `free(payload)` before
  `abort()` (double-panic path and uncaught-panic path). Strictly safe: the
  next statement is `abort()`, so a heap payload is reclaimed by the OS and
  nothing leaks, while a scalar/borrowed payload is no longer freed.
- **`panic_payload_free`** (the sibling flagged by the sweep) -- now frees the
  payload *record* only, never `p->value`. This matches the deliberate design
  of `tur_result_box_free`, which already refuses to free the caught payload
  value because it is opaque (may be an inline scalar or borrowed). Reached on
  the non-abort catch/fiber cleanup paths, so the change removes the same
  scalar-UB hazard there without gating on payload type (the value is opaque at
  this ABI boundary, exactly why the box path refuses the free too).

### Verification

- Minimal repro (`(panic-with 7)` under `catch-unwind`): `gcc -O2 -Wall
  -Wfree-nonheap-object` now emits **0** `-Wfree-nonheap-object` warnings
  (previously fired twice); runs clean under `-fsanitize=address,undefined`.
- **Warning sweep**: compiled the `emit-c` output of every fixture that
  exercises `panic`/`panic-with`/`catch-unwind`/`catch-panic` (73 `.tur`
  inputs) at `-O2 -Wall -Wfree-nonheap-object` in parallel. **Zero**
  `free-nonheap-object` hits survive. Harness validated against a known
  `free((void*)7)` (fires) and confirmed no `free(payload)` remains in emitted
  output.
- Full suite (`bash tests/run.sh`): **1978 passed, 0 failed** after
  regenerating the 132 affected codegen snapshots (diff is exactly the three
  removed frees plus the new `if (p) { free(p); }` body).

# `tur_panic_with` frees an inline-scalar panic payload as if it were heap

`(panic-with <expr>)` lowers to `tur_panic_with(type_tag, (void*)<expr>, ...)`
(`emit_expr.c:2703`). The payload value is **opaque**: it may be a heap pointer
(`panic "..."` mints a heap string) OR an inline scalar reinterpreted as a
pointer (`(panic-with 7)` passes `(void*)7`). But the two **abort paths** in the
`tur_panic_with` preamble free it unconditionally as heap:

- `emit_module.c:6796` -- double-panic path: `free(payload); abort();`
- `emit_module.c:6813` -- uncaught-panic path: `free(payload); abort();`

For a scalar payload this is a `free()` of a non-heap pointer -- undefined
behavior. Because `abort()` follows immediately the practical impact is nil (the
process is dying and the OS reclaims everything either way), but it is UB on the
way out, and under `-O2` gcc constant-folds the literal, proves the pointer is
non-heap, and emits `-Wfree-nonheap-object` on any program that does an uncaught
`(panic-with <int-literal>)`.

## Minimal repro

```turmeric
(defn make [] : (Result int int)
  (catch-unwind (fn [] : int (panic-with 7))))
(defn main [] : int
  (let [r (make)] (if (err? r) 1 0)))
```

```sh
TUR=./build/tur
$TUR emit-c /tmp/p.tur > /tmp/p.c
gcc -std=c11 -O2 -c /tmp/p.c -o /dev/null
# warning: 'free' called on a pointer to an unallocated object '7'
#   [-Wfree-nonheap-object]   (twice: tur_panic_with double-panic + uncaught paths)
```

(The catch-unwind wrapper is only there to give a self-contained `main`; the
warning is about the panic payload, not the catch box. It fires whenever
`(panic-with <scalar>)` reaches an abort path -- i.e. an uncaught panic. At `-O0`
gcc lacks the constant-propagation to prove non-heap, so the warning is
optimization-gated, but the underlying non-heap `free` is present regardless.)

## Root cause

The panic value is opaque -- codegen has no basis to assume it is heap-owned.
This is the same opacity the caught-box path already respects:
`tur_result_box_free` deliberately does NOT `free(p->value)` "since the value is
opaque and may be an inline scalar or borrowed elsewhere"
(`emit_module.c` comment; see also the "Related" note in
`docs/archive/catch-unwind-returned-err-box-payload-leak.md`). The abort paths
in `tur_panic_with` never got that same treatment: they still `free(payload)`.

## Fix direction

Drop the `free(payload)` before `abort()` on both abort paths
(`emit_module.c:6796`, `:6813`). The frees are simultaneously **pointless** (the
next statement is `abort()`, so the OS reclaims the allocation -- a heap payload
does not "leak" in any observable sense) and **unsound** (a scalar or borrowed
payload must never be freed). Removing them is strictly safe and clears the
warning. Do NOT try to gate the free on "is the payload a heap type": the value
is opaque at this ABI boundary, exactly why the box path refuses the same free.

## Sweep for sibling free-of-opaque / non-heap frees -- do this as part of the fix

This is one instance of a class: a runtime/preamble site that `free()`s a value
whose heap-ownership codegen cannot vouch for. When fixing the two abort-path
frees, also audit these and confirm each is either provably heap-owned or made
non-freeing, and (re)build a representative panic/catch program at `-O2` to
confirm the warning class is gone:

1. `panic_payload_free` -> `free(p->value)` (`emit_module.c:6476`). Reached on
   the type-mismatch re-panic cleanup paths (`emit_module.c:6510`, `:6529`,
   `:6883`, `:6902`), NOT an abort. This frees `p->value` -- the same opaque
   panic value -- so a scalar/borrowed panic payload there is the same hazard
   (this is the pre-existing residual the catch-box report flagged as "separate,
   pre-existing"). Decide deliberately: leave the value unfreed (matching the
   box path) or prove these paths only ever carry heap values.
2. Any other emitted `free(...)` whose argument is a `(void*)`-cast opaque handle
   or a value that may be an inline scalar reinterpreted as a pointer.

### Automated warning sweep (the concrete task)

Compile every fixture's `emit-c` output at `-O2` with `-Wfree-nonheap-object`
(and `-Wall`) and collect the hits, so the whole class is enumerated rather than
fixed one repro at a time. A naive per-fixture `gcc` loop over all ~1442
fixtures is too slow to run inline (it times out); make it tractable first:

- restrict to fixtures that actually exercise `panic`/`panic-with`/`catch`
  (grep the `.tur` inputs) before compiling, or
- compile in parallel (`xargs -P"$(nproc)"`), and/or
- reuse one large translation unit where possible.

Fold whatever survives into this report (or its fix) before archiving.

## Related

- `docs/archive/catch-unwind-returned-err-box-payload-leak.md` -- sibling fix;
  its "Related" section first noted that the opaque panic value is never freed by
  `tur_result_box_free` by design. This report is that residual's other half, on
  the panic-emit / abort side.
