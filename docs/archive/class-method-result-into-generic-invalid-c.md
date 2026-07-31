---
status: RESOLVED 2026-07-31 -- consolidation increment 2 (archived)
severity: medium-high
discovered: 2026-07-30
area: compiler (typeclass method result erasure meets generic specialization)
---

# Typeclass method result into a generic parameter: invalid C

## Summary

Feeding a typeclass method's result (a by-value ADT or struct) directly into
a generic function emits invalid C -- `aggregate value used where an integer
was expected` at the specialized call. Either half alone is fine: the method
result matched/field-read directly works, and the generic over a directly
constructed value works. The composition is what breaks. `tur check` passes.

Found by `tests/type-fuzz-src.py` (seed 11 case 93, tags
`adt,ascribe,class_thru,gid`), then hand-minimized -- the ascription is not
needed.

**Resolution:** `fn_body_tail_is_carrier_producer` (emit_fns.c) classified
every `__inst_*` callee as a carrier producer BY NAME -- stale for the M7
by-value path, where a pure-Turmeric instance method with a concrete
by-value product result returns the aggregate. The spec-call arg path then
deref'd that aggregate as a pointer. The classifier now consults the
instance's declared result; suite 2444/0 in isolation; pinned by
`tests/fixtures/class-method-result-into-generic/` (direct ADT + struct
receivers, dotted and bare dispatch, plus the let-bound control).

## Repro

    $ cat > /tmp/n1.tur <<'EOF'
    (defdata FzW (FzWc :int))
    (defclass FzT [a] (thru [self : a] : a))
    (definstance FzT [FzW] (thru [self : FzW] : FzW self))
    (defn gid [A] [x : A] : A x)
    (defn main [] : int
      (println (match (gid (.thru (FzWc -10))) (FzWc x) x))
      0)
    EOF
    $ ./build/tur run /tmp/n1.tur
    error: aggregate value used where an integer was expected
      __auto_type __ps = (gid__spec__tur_adt_FzW_tur_adt_FzW(
                            (*(tur_adt_FzW *)(intptr_t)(__ps_prev))));
    tur: cc invocation failed (status 256)

Same with a `defstruct` receiver and with bare (`thru`) dispatch in place of
dotted (`.thru`).

## Controls

| Variant | Result |
|---|---|
| method result matched directly, no generic: `(match (.thru (FzWc -10)) ...)` | ok |
| generic over a constructed value, no method: `(gid (FzWc -10))` | ok |
| method result LET-BOUND, then into the generic | ok |
| method result through a monomorphic pass-through, then the generic | ok |
| method result -> generic, direct composition | **invalid C** |

## Root cause (direction)

The method result comes back on the int64 carrier (the emitted C shows the
bridge already half-applied: `(*(tur_adt_FzW *)(intptr_t)ps)` re-inflates a
pointer to the by-value aggregate). The generic call site then passes that
aggregate to the specialization, whose call-expression result is consumed as
an integer again -- the carrier->concrete bridge is applied on one side of
the generic call but not the other. Same anatomy as
[result-monad-bind-typed-boundary-miscompiles](result-monad-bind-typed-boundary-miscompiles.md)
(method results carry the erasure; boundaries re-interpret instead of
re-wrapping), surfacing at a generic call instead of a typed defn boundary.

## Fix directions

1. Whatever re-wrap discipline lands for method results (see the fix
   directions in result-monad-bind-typed-boundary-miscompiles) must also
   cover the generic-call argument path: an instance method result flowing
   into a tyvar-typed parameter needs the same carrier->concrete bridge as
   one flowing into a `(Result A B)`-typed binding.
2. Fixture: the three-row control table above.
3. Until fixed, `tests/type-fuzz-src.py` avoids the shape via
   `known_bug_slug` and pins it in `--known-probes`.

## Workaround

Let-bind the method result before the generic call (verified: runs clean) --
the binding applies the carrier->concrete bridge the direct composition
skips. A monomorphic pass-through helper works for the same reason. The
narrowness of the trigger (only the direct composition breaks) is a strong
hint the bridge exists and is simply not consulted on the generic-call
argument path.

## Guide upkeep

When this report is resolved -- or any representation/bridge it describes
changes shape on the way -- update
[docs/guides/value-representations-guide.md](../guides/value-representations-guide.md)
in the same PR: fix the representation inventory, move this report's row out
of the missing-cells table, and correct the link when the report moves to
`docs/archive/`.
