---
status: open
severity: medium
discovered: 2026-07-07
discovered-by: compiled-catch-unwind-general-lowering-plan G5
area: compiled backend / runtime (catch-unwind)
---

# `catch-unwind` leaks its result box (and a caught panic's payload) every call

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
(`docs/archive/compiled-catch-unwind-general-lowering-plan.md`); the stackless
resume segment is a natural place to RC-drop the box once it is known not to
escape, but it inherits this native behavior today on purpose (native is the
correctness oracle for drop behavior, and a premature free would be a
use-after-free the moment the box escapes as the function's `Result`). Note this
result-box leak is distinct from the stackless-only aggregate-box panic-unwind
leak scoped in
`docs/upcoming/catch-unwind-aggregate-followups-plan.md` (Part B).
