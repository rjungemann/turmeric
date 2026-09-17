# A colored generic whose parameter is a tyvar-elemented application is signature-rejected

**Severity: low.** A loud compiler limitation, not a wrong answer: `tur: this
effect operation has no lowering here`, on a program the interpreter runs
correctly. The diagnostic names the `perform` inside the callee and says
nothing about the parameter type that actually evicted it.

**Status:** open. Found 2026-09-16 while closing
[colored-call-inside-match-evicts-the-cps-backend](../archive/colored-call-inside-match-evicts-the-cps-backend.md)
and wiring the CPS deferred-drop table's tail arm; it is the reason that arm
is unreachable today (see that report's resolution).

## Repro

```turmeric
(defeffect Tick [] : int)
(defn peek [E] [r : (Result int E)] : int
  (perform (Tick))
  (if (ok? r) 1 0))
(defn main [] : int
  (println (handle (peek (ok 1)) (Tick [] k) (resume k 1)))
  0)
```

```
$ TUR_TRACE_EVICT=1 tur check p.tur
[EVICT] SIG-REJECT  eff=1 peek
$ tur run p.tur          # error: this effect operation has no lowering here ...
$ tur --interpret p.tur  # 1
```

The control is the same function with the arm pinned: `[r : (Result int
cstr)]` lowers and prints `1`. So it is the type VARIABLE in the parameter,
not the Result, not the perform.

## Root cause

`fn_sig_ok` (`src/compiler/emit_cps_ir.c`) gates a colored function's
parameters and return through `sig_slot_ok`, which admits a scalar or a
CONCRETE application whose C spelling is the int64 carrier, and refuses a
tyvar-elemented application on purpose. Its own comment records why:

> CONCRETE only, deliberately: a tyvar-elemented app (`(Option A)`, `(Map A
> B)`) must keep rejecting, because the whole mono-template / island
> machinery is built on the generic BASE sig-rejecting (see the "sig-rejects
> itself so in_s stays false" invariants); admitting it flipped map-eq
> drivers to candidates and double-emitted their CPS joins.

So a generic colored function's BASE is never CPS-emitted; only its
monomorph clones are, through the G3b mono-template path -- and `peek` here
has no concrete clone to route to, because nothing pins `E`: `(ok 1)` is
`(Result int ?)` and stays an erased carrier box. The base is the only
candidate, and the base sig-rejects.

This is the same erasure that makes the deferred-drop table's cps->cps tail
arm unreachable: an erased box needs an unpinned tyvar, and the only
colored callee that could take it is exactly this shape.

## Why it is not a two-line fix

Admitting the tyvar-elemented app to `sig_slot_ok` was measured and reverted
when the SR2b carrier rule landed (the comment above): the mono-template
machinery keys on the base being sig-rejected, and the map-eq drivers were
double-emitted. Any fix has to either

1. mint a concrete clone for the unpinned call (bind `E` to a default --
   the int carrier -- at the call site, the way the elaborator already
   defaults an un-annotated parameter), so the G3b path has something to
   route to and the base stays rejected; or
2. teach the island machinery that a base CAN be emitted for a
   carrier-erased signature without becoming a candidate for the clones'
   joins -- the invariant the comment names, which is the larger change.

Direction 1 is the cheaper one and matches how the direct emitter already
treats the same call (it takes the carrier base). It needs the clone's
signature to spell the erased arm as `int64_t` on both the `__cps` entry and
the direct wrapper, which `sig_slot_ok`'s SR2b arm already accepts once the
app is concrete.

## Scoping note, 2026-09-17

Direction 1 was scoped and not started; what it needs is recorded so the
next pass does not re-derive it:

- **The elaborator cannot gate on "colored".** `cps_colored` is written by
  `cps_color_program` (`src/passes/cps.c`), which `ensure_S` in
  `emit_cps_ir.c` runs AFTER the emitter's ABI pre-scan has already populated
  `ctx->abi_specializations` -- the spec set the G3b mono-template
  classification then reads.  At the call site in `elab_call.c` only the
  DECLARED effect row (`type.as.fn.effect_row`, from a `#fx{}` annotation) is
  available; `FnDef.inferred_effect_row` is NULL until the P19-2 inference
  pass runs.  So "bind `E` to the int carrier at the call site for a colored
  callee" has no colored signal to key on at elab time.
- **The binding is not absent, it is abstract.** `(peek (ok 1))` unifies
  `(Result int E)` against `(Result int B)` (`ok`'s own result tyvar), so
  `E -> B` IS collected -- as a TYVAR-typed binding, which the emitter's
  `emit_abi_type_has_concrete_named_tyvar` reads as "route through the relay
  path" (the carrier base).  The defaulting therefore belongs on the EMIT
  side (`emit_abi_register_call`, where the abstract binding is recognised)
  or after the coloring pass, not in `elab_call.c`; the Saffron `any`
  defaulting in `elab_call.c` (D8 Q3) is dialect-keyed and does not carry
  over.
- The pinned-arm control is unchanged and still prints `1`.

## Workaround

Pin the arm: declare the parameter at a concrete type (`(Result int cstr)`),
or reduce the Result to a scalar in the caller before the effectful call.

## Fixture owed

The repro above, plus the pinned-arm control, asserting `1` twice; the
deferred-drop tail arm in `emit_cps_ir.c` becomes reachable by it and wants a
leak-check marker.
