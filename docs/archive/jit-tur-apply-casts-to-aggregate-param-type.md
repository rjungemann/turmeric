# `TUR_APPLY<N>_T` casts arguments to a struct type (invalid C)

**RESOLVED 2026-07-28 in `b61cdf578`.** Fixed as the "conditional on `A0`
being scalar" option below: the emitter now decides per argument, keeping the
cast for scalars and omitting it for aggregates.  The all-scalar case still
emits the macro verbatim, so stdlib's hand-written inline-C users are
untouched.  `tests/run.sh` 2399 passed / 0 failed; JIT full corpus
1557 -> 1559 (92.8%); zero snapshot churn.

The load-bearing question the "Fix directions" section raises below was
answered by *not* answering it: rather than determine whether the cast is
needed for the int64 -> pointer direction (unverifiable here -- gcc 14 makes
it an error and this box has gcc 13.3), the cast is simply kept wherever it
was valid.  Only the provably-inert aggregate case changed.

Cost worth carrying forward: the aggregate path spells out the macro's own
expansion, so `emit_expr.c` and the macro definition in `emit_module.c` must
now stay in sync.  Both carry a comment pointing at the other.

Original report follows.

---

**Severity: low today, latent portability defect.** 2 fixtures; no known
miscompile on the `cc` path. Found by the J0 JIT sweep, not by the suite.

## Summary

The emitted typed-apply macros cast each argument to its parameter type:

```c
#define TUR_APPLY1_T(R, A0, f, a) \
    (((R (*)(void *, A0))(intptr_t)TUR_CLOSURE_FN(f)) \
        ((void *)(intptr_t)(f), (A0)(a)))
```

`(A0)(a)` is fine while `A0` is scalar. When the parameter type is an
**aggregate** it expands to a cast to a struct type, which C does not permit --
a cast operand and target must both be scalar (C11 6.5.4p2). gcc and clang
accept it silently; c2mir rejects it:

```
conversion to non-scalar type requested
```

## Repro

```sh
./build/tur emit-c tests/fixtures/dot-receiver-first-call/input.tur > /tmp/d.c
grep -n 'TUR_APPLY1_T(const char \*, tur_adt_Person' /tmp/d.c
```

```c
const char * __ps_161 =
    (TUR_APPLY1_T(const char *, tur_adt_Person, (int64_t)(b_1307).get, p_1306));
```

`A0` is `tur_adt_Person`, a by-value struct, so the expansion contains
`(tur_adt_Person)(p_1306)`.

Affected fixtures: `dot-receiver-first-call`,
`dot-parametric-fn-field-call`.

## Why the suite never caught it

`tests/run.sh` compiles with gcc/clang, both of which accept a struct cast
whose operand already has that type (it is a no-op there). The construct only
surfaces against a front end that enforces the standard. This is the same shape
as the `__extension__` defect in
[jit-macos-full-corpus-extension-and-atexit.md](jit-macos-full-corpus-extension-and-atexit.md):
our C is less portable than the two compilers we habitually test with, and a
stricter front end is what exposes it.

It also stayed hidden inside the JIT sweep until S1 landed. Both fixtures used
to fail earlier, on an unresolved `__auto_type` parse error; fixing that let
c2mir reach this line. Unmasking, not regression -- worth noting because the
sweep's failure-class tally shifts for that reason and a new class appearing is
not by itself evidence of a new bug.

## Fix directions

The cast looks redundant. The callee is called through a *prototyped* function
pointer `R (*)(void *, A0)`, so the argument is already converted to `A0` by the
usual assignment conversions at the call. Removing it entirely is the smallest
change:

```c
#define TUR_APPLY1_T(R, A0, f, a) \
    (((R (*)(void *, A0))(intptr_t)TUR_CLOSURE_FN(f)) \
        ((void *)(intptr_t)(f), (a)))
```

Before doing that, check whether the cast is load-bearing for the int64
carrier -> pointer direction: `-Wint-conversion` is an error on gcc 14, and if
some call site passes an `int64_t` where `A0` is a pointer, the explicit cast is
what silences it. If so, the cast has to become conditional on `A0` being
scalar, which a macro cannot decide -- the emitter would need to choose between
a casting and a non-casting spelling per call site, based on the parameter type
it already knows.

Verify with a full `bash tests/run.sh` plus a JIT sweep
(`bash tools/jit-spike/sweep-full.sh`); the two fixtures above are the check.

## Provenance

docs/upcoming/jit-engine-j0-findings.md, S1 measurement pass (JIT full corpus
92.7%). Reported rather than fixed in place: it edits a macro every closure
call goes through, and the change wants its own validation cycle rather than
riding along at the end of another one.
