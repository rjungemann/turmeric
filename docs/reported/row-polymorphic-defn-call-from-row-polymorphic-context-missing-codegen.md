---
title: Row-polymorphic defn called from another row-polymorphic defn fails to emit C declaration
category: Codegen / monomorphization
status: FIXED -- the relay-vs-carrier classifier (`emit_abi_call_is_generic_relay` in `src/compiler/emit_module.c`) now ignores row-kinded (`KIND_TYPEROW`) named tyvars when deciding whether a call requires specialization. Rows are phantom and do not change the C ABI, so the carrier definition of the callee is sufficient and now gets emitted. Regression covered by `tests/fixtures/hkt-row-polymorphic-call-from-polymorphic`.
severity: WAS A program that elaborates cleanly fails to link with an implicit-function-declaration error from cc. Hard error at link time, not a silent miscompile, but the diagnostic surfaces in cc output rather than from the Turmeric front-end, which makes it look like a compiler-internal failure to the user.
description: A `defn` declared with row-kinded type parameters (`^&in ^&out`, L6 follow-up B) elaborates fine and is callable from a *concrete-row* call site; but when called from another row-polymorphic `defn`, no C definition was emitted for the callee. The caller's C body referenced the symbol and cc failed the build.
---

# Row-polymorphic `defn` not emitted when called from a row-polymorphic context

> **UPDATE: fixed.** See the "Fix" section at the bottom.

## Summary

L6 follow-up B added `^&name` row-kinded type parameters to `defn`/`fn`.
A row-polymorphic `defn` can be defined and called from a *concrete-row*
call site (the row arguments are pinned, monomorphization sees concrete
types). When the call site is itself row-polymorphic — the caller also
has `^&in ^&out` and the row arguments pass through unchanged — no C
definition is emitted for the callee. cc then fails:

```
error: call to undeclared function 'ecs__query__query_hyworld'; ...
```

## Minimal repro

```turmeric
;; row-polymorphic accessor on a row-typed Query value
(defstruct Query [^&in ^&out] (world :int))

(defn query-world [^&in ^&out] [q : (Query in out)] : int
  (.world q))

;; row-polymorphic caller
(defn extract [^&in ^&out] [q : (Query in out)] : int
  (query-world q))    ;; <-- triggers the missing-symbol cc error

(defn main [] : int
  (let [q (:: (make-struct Query 7) (Query #row{int} #row{int}))]
    (println (extract q))
    0))
```

Build with `-Xdata-literals`:

```
$ tur run /tmp/repro.tur
/tmp/tur-build/.../repro_tur.c:4632:16: error: call to undeclared function
  'ecs__query__query_hyworld'; ISO C99 and later do not support implicit
  function declarations [-Wimplicit-function-declaration]
        return ecs__query__query_hyworld(q);
                ^
tur: cc invocation failed (status 256)
```

## Observed vs. expected

**Observed.** No emitted definition for `query-world` reaches the
generated C TU; cc reports an implicit-function-declaration error and
the build fails.

**Expected.** A row-polymorphic call from another row-polymorphic
caller should preserve the row variables through the call boundary
(rows erase at codegen, so the C signatures are identical regardless
of row instantiation) and emit one shared C function. This is the
phantom-row property that backs the rest of the row work — there is
no per-instantiation C variant, the row arguments contribute zero
runtime payload.

## Workaround

Replace the call with a direct field access:

```turmeric
(defn extract [^&in ^&out] [q : (Query in out)] : int
  (.world q))          ;; OK
```

Concrete-row callers work without change:

```turmeric
(defn extract-pv [q : (Query #row{Pos Vel} #row{Pos})] : int
  (query-world q))     ;; OK -- monomorphizes against the concrete rows
```

## Root cause (provisional)

Monomorphization keys on the type arguments at the call site. For row
arguments that are themselves type variables (the caller's `^&` params),
the monomorphizer either does not register a need for the callee or
deduplicates to "no instantiation" because the rows are phantom and
the C signature is identical regardless. The emit-phase walk then
skips the callee because no concrete instantiation was recorded.

The fix is presumably: row-polymorphic callers should drive emission
of a single canonical (row-erased) C definition of the callee, since
all row instantiations produce the same C function.

## Proposed validation

A fixture `tests/fixtures/hkt-row-polymorphic-call-from-polymorphic`
that compiles + runs the minimal repro above and expects "7" on
stdout. Currently it would fail with the cc error described.

## Related

- `docs/archive/history/variadic-hkt-rows-missing.md` -- the L6
  follow-up B documentation; the row-polymorphic `defn` surface
  itself works for concrete callers.
- `tests/fixtures/hkt-row-polymorphic-defn` -- positive fixture that
  exercises a concrete-row call (passes).
- `tests/fixtures/hkt-row-polymorphic-call-from-polymorphic` -- the
  regression fixture added with the fix.
- `../turmeric-spices/spices/ecs/src/ecs/query.tur` (E1) -- the
  Query value's `query-world` accessor; the spice workaround
  (`.world` field access in row-polymorphic callers) has been
  removed now that the fix landed.

## Fix

The relay-vs-carrier classifier in `emit_abi_call_is_generic_relay`
(`src/compiler/emit_module.c`) walked each call's abi bindings and
treated the call as a relay (i.e. *the callee will be specialized
once this caller is specialized; no carrier emission needed*) as
soon as one binding contained a named tyvar.  For row variables that
premise is wrong: rows erase at codegen, every row instantiation
produces the same C function, so the carrier emission *is* the
specialization.  Treating the call as a relay therefore both
suppressed the carrier-call note (which would have driven emission
of the callee's carrier definition) and produced no real
specialization downstream, leaving the call dangling.

A new helper `emit_abi_type_has_concrete_named_tyvar` is identical
to `emit_abi_type_has_named_tyvar` except that it skips `TY_TYVAR`
nodes whose `hkt_kind == KIND_TYPEROW`.  The relay decision uses
this new helper; the rest of the type-walker (TY_APP, TY_UNION,
TY_INTERSECTION) keeps its existing semantics.

Net effect: a row-poly call from a row-poly context now notes the
callee as a carrier call, emit emits the callee's carrier
definition, and the cc link error disappears.  Non-row generic
relays (the original use case the gate was added for) are
unaffected: their abi bindings still contain at least one
non-row-kinded named tyvar.
