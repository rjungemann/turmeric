---
status: resolved
severity: medium
discovered: 2026-07-07
discovered-by: compiled-catch-unwind-general-lowering-plan G5
resolved: 2026-07-07
area: compiled backend / runtime (catch-unwind)
---

# `catch-unwind` leaks its result box (and a caught panic's payload) every call

## Resolution (2026-07-07)

The safe, sound half of the report's first fix direction landed: a
**statement-position** `catch-unwind` / `catch-panic-of` -- one whose Result is
provably discarded -- now frees the caught Result box and, on the err branch,
the caught `tur_panic_payload` record, at the end of the statement.

- New runtime helper `tur_result_box_free` (`src/compiler/emit_module.c`, in the
  panic-runtime preamble beside `tur_catch_unwind_box`): frees the
  `tur_result_box_t`, and for an err box also frees the `tur_panic_payload`
  *struct*. It deliberately does **not** free `payload->value` -- a `panic-with`
  value can be an inline scalar (`(void*)42`) or a value owned elsewhere, so
  freeing it is a nonheap-free / double-free hazard. This is why
  `panic_payload_free` (which does `free(p->value)`) is not reused here; calling
  it on an inline-scalar payload aborts.
- `emit_stmt.c` `EX_CATCH_UNWIND` / `EX_CATCH_PANIC_OF` (statement position)
  now emits a `tur_result_box_free(...)` call after the catch. The value is
  provably discarded there, so this is not a premature free; a caught Result
  that is *used* (let-bound, returned, inspected) flows through `emit_value` and
  is untouched.

### What this fixes

- The **unbounded caught-panic payload leak**: a `catch-unwind` in a loop that
  catches and discards now frees the 32-byte payload each iteration instead of
  leaking it. Measured on the repros:
  - single panic-discard: 48 B lost -> 16 B lost;
  - 3 sequential panic-discards: 144 B -> 48 B;
  - 1000-iteration loop of caught+discarded panics: ~48 KB -> 16 KB.
- The Result box in statement position is now explicitly freed (it was in
  practice DCE'd by the C compiler when the box pointer was unused, so it was
  not the block valgrind actually reported -- see below).

### What remains (spun out to a fresh report)

The residual `definitely lost` on every catch site is the **fat-closure thunk**
materialized for the `(fn [] ...)` argument (16 bytes / call), plus the
still-open **let-bound-box** case where a caught Result is used and then goes out
of scope without escaping (needs the escape/last-use check the report flags as
an open design question). Both are tracked in
`docs/reported/catch-unwind-thunk-closure-leak.md`. Note the report's original
valgrind attribution of the 16-byte block to "the `tur_result_box_t`" was a
misread: `tur_result_box_t` is 24 bytes and, in the discard case, was DCE'd; the
16-byte block traced to `main` is the thunk fat struct.

---

## Summary

Every `catch-unwind` in the compiled backend heap-allocates a `tur_result_box_t`
that is **never freed** -- `definitely lost` under valgrind. When the thunk
actually panics, the caught `tur_panic_payload` is boxed into the `err` result
and also never freed. The leak is **per catch site**, so a `catch-unwind` in a
loop or a deep recursion leaks unboundedly. This is a **native (flag-off),
always-on** behavior -- not specific to the `stackless-catch-unwind` experiment,
which merely mirrors it.

Severity is medium: it is not a regression (long-standing), bounded per catch
site, and only bites long-running / high-iteration programs; but it is a genuine
unbounded leak in shipping codegen.

## Minimal repro

```turmeric
(defn main [] : int
  (catch-unwind (fn [] : int 5))   ; result discarded
  0)
```

```sh
TUR=./build/tur
$TUR build /tmp/one.tur -o /tmp/one
valgrind --leak-check=full /tmp/one
# ==> definitely lost: 16 bytes in 1 blocks   (the tur_result_box_t)
```

Three sequential catches leak three boxes (`48 bytes in 3 blocks`), confirming
it is per-catch. A caught panic leaks box **plus** payload:

```turmeric
(defn main [] : int
  (catch-unwind (fn [] : int (panic-with 42)))
  0)
;; ==> definitely lost: 48 bytes in 2 blocks  (16 box + 32 payload)
```

## Root cause

`tur_catch_unwind_box` (emitted at `src/compiler/emit_module.c:6571`) returns
`tur_box_ok(v)` / `tur_box_err(payload)` where `tur_box_ok`/`tur_box_err`
(`emit_module.c:5615` / `5621`) `malloc` a `tur_result_box_t` and hand back the
pointer. Nothing on the consuming side frees it: the caught result is typically
predicated (`ok?`/`err?`) or extracted (`ok-val`/`err-val`) and then dropped,
and no drop/free is inserted for it. The `err` branch additionally keeps the
`panic_payload_new` record (`emit_module.c:6475`) alive by boxing its pointer
(`emit_module.c:6580`) with no later free.

`stdlib/option.tur` has `option-free` and there is a matching result-free path,
so the box *can* be freed manually -- but an implicitly-created caught result
that is discarded has no natural owner to call it, and the language does not
insert an automatic drop here.

## Fix directions

Open design question first: is the caught `Result` box meant to be
**caller-freed** (manual RC -- then this is a docs/lint gap, and the compiler
should at least free a *discarded* caught result), or is its lifetime the
**codegen/runtime's** responsibility (then `catch-unwind` should arena or
RC-drop the box once its last use is past)? Options:

- Free a caught result that is provably discarded (statement-position
  `catch-unwind`, or a `let`-bound result whose scope ends without escaping) at
  the end of its scope. Needs a small escape/last-use check so a returned/stored
  `Result` is not freed early.
- Arena the boxes + payloads per catch-tree and release the arena when the
  outermost boundary pops.

For the `stackless-catch-unwind` lowering specifically, the same fix was the
deferred "no-leak" item under G5 of the now-archived general lowering
(`docs/archive/history/compiled-catch-unwind-general-lowering-plan.md`); the stackless
resume segment is a natural place to RC-drop the box once it is known not to
escape, but it inherits this native behavior today on purpose (native is the
correctness oracle for drop behavior, and a premature free would be a
use-after-free the moment the box escapes as the function's `Result`). Note this
result-box leak is distinct from the stackless-only aggregate-box panic-unwind
leak scoped in
`docs/upcoming/catch-unwind-aggregate-followups-plan.md` (Part B).
