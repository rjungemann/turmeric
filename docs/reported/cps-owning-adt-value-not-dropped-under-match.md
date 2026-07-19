# Matched boxed ADT / handler value not dropped (general owning-value teardown gap)

**Severity:** low (small, one-shot leaks near program end; the suite's run phase is
not leak-checked for these fixtures, so they pass). **NOT CPS-specific** -- see the
correction below. The original title said "on the CPS path"; that framing was
wrong and is kept only as the filename.

## Symptom

```sh
CC="cc -fsanitize=address,undefined -g" \
  ./build/tur build tests/fixtures/cps-backend-effect-under-match/input.tur -o /tmp/e
ASAN_OPTIONS=detect_leaks=1 /tmp/e
# => Direct leak of 16 byte(s): malloc <- ctor_Full <- route__cps <- run__cps
```

A boxed `Full` ADT is constructed (`(Full 7)`), passed to `pick`, matched to
extract its int, and the box is never freed. Output is correct; only the box leaks.

Second instance -- `defstruct-field-handler` / `-multi` / `fh-multi-effect-type`:
a first-class handler value (`(handler (Ask [] k) ...)`) is allocated via
`tur_handler_table_new(1)` (+ its `entries` array), stored in a `defstruct` field,
installed via `with-handler`, and never freed (16 + 40 bytes; `tur_handler_table_free`
is emitted but never called).

## Root cause -- CORRECTED (2026-07-19)

The earlier "the direct emitter applies the drop, the CPS path does not" premise is
**FALSE**, verified by a minimal NON-effect probe:

```turmeric
(defdata Box (Empty) (Full :int))
(defn pick [b : Box] : int (match b (Full v) (+ v 1) (Empty) 0))
(defn main [] : int (println (pick (Full 7))) 0)
```

This program has no effects, no CPS/DK lowering -- it goes through the ordinary
direct emitter -- and it leaks the **same 16 bytes** from `ctor_Full`. So the box
is not dropped on EITHER path: Turmeric simply does not insert a scope-exit drop
for a matched-and-consumed **non-linear** boxed ADT. Likewise a first-class handler
value is never freed on any path (handler values only ever existed in effect code;
pre-graduation the fiber path also `tur_handler_table_new`'d and never freed it).

So both leaks are a **general owning-value teardown gap in the ownership/drop
subsystem**, present regardless of the CPS/DK endgame. They were merely *surfaced*
by Stage G making these fixtures DK-lower end to end; they are NOT a regression
introduced by the CPS work, and the fix does not live in the CPS emitter.

Open question for the language owner: is a matched non-linear boxed ADT *meant* to
be dropped? If ADT values are owned (drop-on-scope-exit), this is a real bug in
`match` teardown (both emitters). If non-linear ADTs are intentionally
value/leak-semantics and only `^linear`/`^unique` ones are freed, then only the
handler-value leak (which has no ownership annotation path) is a genuine defect.

## Fix direction

Language-level, not CPS-level: insert the scope-exit drop for an owning value
(boxed sum ADT with a heap box, heap struct, handler value) that is consumed by a
`match` or dies at a `let`/function scope boundary, in the **direct emitter's**
drop-glue pass (which the CPS/DK path already piggybacks on for the cases it does
handle). Respect linearity (a `^linear` value is single-use; a shared/`^borrow`
value must not be freed) and multi-shot resume (a value read-only-shared across
resumes must not double-free). For handler values specifically, give `TY_HANDLER`
drop glue (`tur_handler_table_free`) and drive it from the same scope-exit pass.
This is the Phase-3 owning-value teardown work
(`cps-backend-multishot-continuations-owning-capture-plan.md` E3/E4, OPEN); it is
independent of the fiber-effect-runtime deletion, which is complete.
