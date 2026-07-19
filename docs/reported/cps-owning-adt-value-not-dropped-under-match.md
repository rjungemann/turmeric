# Owning ADT value not dropped on the CPS path (16-byte ctor_Full leak)

**Severity:** low (experimental `--enable=cps-tramp-resume` path only; correctness
fine -- a single owning ADT value leaks once). Distinct from the resolved
perform-cont-frame leak; this is the Phase-3 owning/linear-value scope-exit drop
gap on the CPS delegation path (E3/E4 in
`cps-backend-multishot-continuations-owning-capture-plan.md`, still OPEN). Matters
for a leak-checked flag-on suite / flag graduation.

## Symptom

```sh
CC="cc -fsanitize=address,undefined -g" \
  ./build/tur --enable=cps-tramp-resume build tests/fixtures/cps-backend-effect-under-match/input.tur -o /tmp/e
ASAN_OPTIONS=detect_leaks=1 /tmp/e
# => Direct leak of 16 byte(s):
#      malloc <- ctor_Full <- route__cps <- run__cps <- run
```

The value is a boxed `Full` ADT constructed in `route` (a match arm), threaded
through `route__cps` / `run__cps`, and never freed. Output is correct; only the
owning value leaks.

## Root cause (direction)

Flag-off the direct emitter applies the owning-value scope-exit drop for a boxed
ADT that is constructed, consumed, and goes out of scope. On the CPS delegation
path the constructed value crosses the DK slot / cps->cps call as a plain int64
carrier, but the scope-exit drop the direct emitter would insert is not applied,
so the malloc'd `Full` box leaks. This is the same owning/linear teardown gap that
the B8 session-effects work hit for the `^linear` session channel
(`cps-b8-session-effects-plan.md` "the leak is ... the `^linear` session channel's
scope-exit drop, which the CPS delegation path does not apply"); the perform-cont
frame leak (now resolved) was a different, DK-node-lifetime bug.

## Second instance (2026-07-19): owning handler VALUE table not freed

`defstruct-field-handler` (and `-multi`, `fh-multi-effect-type`) allocate a
first-class handler value via `tur_handler_table_new(1)` (+ its `entries` array),
store it in a `defstruct` field, install it via `with-handler`, and never free it:

```
Direct leak of 16 byte(s):   tur_handler_table_new  (the table)
Indirect leak of 40 byte(s): tur_handler_table_new  (its entries array)
```

Same owning-value teardown gap: a heap-allocated handler value that dies at scope
exit is not dropped on the CPS path. Surfaced (not caused) by the Stage-G fiber
effect runtime deletion, which made these fixtures DK-lower end to end. The DK
handler install (`dk_hgroup_from_table`) reads the table but does not own/free it;
the value needs the owning-carrier scope-exit drop like any other.

## Fix direction

Apply the direct emitter's owning-value scope-exit drop on the CPS delegation path:
a DK-lowered value whose type is an owning carrier (boxed ADT / heap struct /
`^linear`) and that dies at a scope boundary must be freed exactly as the direct
emitter frees it (drop-glue), respecting single- vs multi-shot resume (a multi-shot
resume shares the value read-only and must not double-free). This is the general
Phase-3 owning-value teardown work; closing it makes the remaining flag-on effect
fixtures leak-clean, the last correctness item before a leak-checked flag-on suite
is green.
