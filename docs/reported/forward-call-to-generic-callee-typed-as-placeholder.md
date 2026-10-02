# A forward call to a generic callee is typed as the carrier placeholder

**Severity: medium.** An expressiveness gap: a correct program is refused at
`tur check`.  Until 2026-10-02 the top-level half of it was a silent wrong
answer instead; that half is now this refusal.  Filed 2026-10-02 while fixing
[forward-call-to-aggregate-result-types-as-carrier](../archive/forward-call-to-aggregate-result-types-as-carrier.md).

## Repro

```turmeric
(defn wrap-once [x : int] : (Option int)
  (wrap x))
(defn wrap [A] [x : A] : (Option A) (some x))
```

```
error [TUR-E0709]: function 'wrap-once' declares return type '(Option int)'
but its body returns int
```

Defining `wrap` first compiles.  Inside a `defmodule` it is the same.

## Mechanism

A caller elaborated before its callee sees the pass-1 forward declaration
(`elab_pre_declare_toplevel_defn`, `elab_forward_declare_defns`).  For a
GENERIC callee that declaration has the right arity but no parameter types --
`fwd_decl_scan_params` records only scalar kinds -- and no type parameters,
so nothing at the call can instantiate `A`.

Both pre-passes therefore keep the `TY_INT` placeholder for a result that
mentions the defn's own type parameters (`fwd_type_mentions_tp`).  Before
2026-10-02 the top-level pre-pass committed `(Option A)` anyway, and that was
worse than the refusal: with `(some (wrap x))` above `wrap` and a declared
`(Option (Option int))` result, `A` stayed unbound, `wrap` was emitted only as
its carrier base, and the caller read the `(Option A)` carrier box (16 bytes)
as a by-value `(Option (Option int))` -- an out-of-bounds read; the program
printed nothing at all for the `match` on it (gcc: `-Warray-bounds`).  A dynamic
file keeps committing such results: its forward decl carries closed parameter
types (`elab_fwd_param_full_types`).

## Fix directions

1. Forward-declare a generic callee in full: its type parameters (and kinds),
   its parameter types, its constraints.  That is most of `elab_defn`'s
   signature half, which would have to be separable from the body half.
2. Defer: elaborate a defn whose body calls a not-yet-elaborated generic
   binding after the callee, the way the top-level driver already defers a
   defn that needs a later `definstance` (`tl_deferred`, with the capture
   frame and `n_file_scope_defs` rollback).
3. Elaborate the callee on demand at the first forward call.

Whichever lands, `tests/fixtures/forward-call-aggregate-result-in-module`
has the non-generic shapes; the generic one needs a positive fixture of its
own, with a by-value result so a wrong representation cannot pass silently.
