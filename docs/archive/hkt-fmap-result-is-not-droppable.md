---
status: resolved
severity: low
discovered: 2026-07-26
resolved: 2026-07-26
area: compiler (HKT result typing, elab_typeclasses.c)
---

# `(fmap r f)` over an `rc<T>` returns an untyped `(type-app ? ?)`, so it cannot be dropped

## Summary

`Functor [rc]`'s `fmap` allocates a fresh `rc` for its result -- that is what
`fmap :: (a -> b) -> rc a -> rc b` means. But the call's result type comes back
as the abstract `(type-app ? ?)` rather than `rc<b>`, so there is no way to
release it:

    error: rc/drop requires rc<T> or a constrained existential,
           got (type-app ? ?)

The value is usable (it can be folded, passed on), just not droppable. Every
`fmap` over an `rc` therefore leaks one control block plus one payload slot.

## Repro

    (load "stdlib/rc.tur")

    (defn dbl [x : int] : int (* x 2))
    (defn add [a : int b : int] : int (+ a b))

    (defn main [] : int
      (let [r (rc/of 21)]
        (let [m (fmap r dbl)]
          (println (:: (foldl m 0 add) int))   ; 42 -- the value is fine
          (rc/drop m)))                        ; error: got (type-app ? ?)
      0)

Note the fold works, so this is specifically about the result *type*, not the
value. `(:: (foldl m 0 add) int)` needs its own ascription for the same
underlying reason.

## Severity

Low. It was previously unreachable -- `rc.tur` did not compile at all until
2026-07-26 (`docs/archive/history/rc-tur-legacy-instances-do-not-compile.md`), so no
program could have hit this. Nothing in stdlib uses `fmap` over an `rc`, and
`rc.tur` is opt-in. Filed so the hole is on the record now that the module can
actually be loaded.

## Resolution (2026-07-26)

Fixed. `(rc/drop (fmap r f))` type-checks and the allocation is reclaimed:
`tests/fixtures/hkt-fmap-rc-result-droppable` measures 0 bytes over 5000
fmap/drop pairs.

**The suspected root cause below was wrong in an instructive way.** The
substitution is not broken -- it produces `(type-app rc<?> int)` with both
positions correctly resolved. Traced with a temporary probe at the substitution
site:

    [M7] rft=(type-app rc<?> tyvar) subst=(type-app rc<?> int)
         rt=(type-app ? ?) byval_ok=0 intcarrier=0 free=0

`subst` is right; `rt` (what the call node ends up with) is the def-less shell.
So nothing failed to *unify* -- the grounded result was computed and then
discarded, because the commit gate

    if (m7_body_byvalue_ok) { result_type = substituted; ... }
    else if (result_type.kind == TY_APP && m7_result_is_int_carrier(substituted))

matched neither arm. `m7_body_byvalue_ok` is false because `Functor [rc]`'s body
is **inline-C**, and `m7_result_is_int_carrier` only recognizes opaque/transparent
int newtypes, not a pointer-family head.

**This is not rc-specific**, which the original report also got wrong. Isolated
by holding the container fixed and varying only the body's implementation
language -- a `(defstruct Box [A] (val A))` with a pure-Turmeric `fmap` body
grounds to `(type-app Box int)`, and the *same* container with an inline-C body
degrades to `(type-app ? ?)`. Any HKT instance with an inline-C body loses its
result type. rc.tur is simply one such instance, and necessarily so: rc payload
extraction needs `rc_get_value`, which has no pure-Turmeric accessor.

### What was changed

`m7_app_to_ptr_family` (`src/compiler/elab_typeclasses.c`) collapses an applied
result whose head is a pointer-family builtin -- `(type-app rc<?> int)` -->
`rc<int>` -- with `type_rc_adt`/`type_weak_adt` for an aggregate element so field
access through the handle still resolves. Committed as a third arm beside the
existing two, ahead of the `m7_body_byvalue_ok` arm (which would otherwise commit
the same un-droppable TY_APP shape for a pure-Turmeric body).

`m7_byvalue_grounded` deliberately stays false, so dispatch remains on the
uniform carrier ABI and no by-value spec is minted. That is sound for the same
reason the neighbouring int-carrier-newtype arm is, and more strongly: an rc/weak
is an `RcControlBlock *` and a ref is a plain pointer, so the by-value
representation IS the int64 carrier the method returns -- 8 bytes, same bits, no
aggregate layout for a consumer to misread. The emitter already leans on this
(TY_RC is in emit_fns.c's typed-pointer return escape-hatch list, pinned by
`tests/fixtures/inline-c-rc-return-typed`).

One emit-side follow-on was required. Refining the call node's type past the
callee's signature makes the `__auto_type __ps_N` panic temp record itself as
`RcControlBlock *` while the carrier base really returns `int64_t`, which hides
the straddle from the binder-init bridge and produces
`RcControlBlock *m = __ps_N;` -- a `-Wint-conversion` in the user's own build,
the exact complaint `inline-c-rc-return-typed` was filed about. Fixed in
`emit_expr.c` by recording the carrier for this shape. The first attempt keyed on
"callee's declared return c-names to int64_t", which is far too broad: a
by-value **specialized** callee (`vec_new__spec__tur_adt_Vec__int__`) genuinely
returns the concrete pointer while its generic signature still c-names to the
carrier, so that version added a redundant cast at every such site -- 140
fixtures of churn. Narrowed to "call node's type is a pointer-family handle AND
the callee's declared result is an applied `(f b)`", which is exactly the
refinement's own shape: zero snapshot churn, suite 2368 passed / 0 failed.

### Still open

- **The general inline-C HKT gap remains.** An instance over a non-pointer
  container with an inline-C body still yields `(type-app ? ?)`. Fixing it means
  separating "what type does this call have" from "which ABI does this dispatch
  use", and the surrounding comments record several silent-miscompile bugs from
  getting that wrong -- out of scope here. Filed as
  `docs/reported/hkt-inline-c-instance-body-loses-result-type.md`.
- **`Foldable [rc]`'s bodies still need their inline-C.** Fix direction 2 below
  assumed this fix would unblock a pure-Turmeric rewrite; it does not. That
  needs the *parameter* side -- `(t a)` unifying with an `rc<A>` parameter --
  which is a different code path (`call_collect_type_bindings`) and untouched.
- **`(foldl m 0 add)` still needs an `(:: ... int)` ascription.** A bare-element
  result (`b`) is a different method shape from an applied one (`(f b)`) and does
  not go through the arm added here.

The original report follows for the record; its "Root cause (suspected)" section
is superseded by the above.

## Root cause (suspected, not confirmed) -- SUPERSEDED, see Resolution

The class method's declared result is `(f b)` over the class's type constructor
variable. Instantiating the instance head `[rc]` should ground that to `rc<b>`,
but the result stays a `TY_APP` with both positions unresolved. This is adjacent
to the gap noted in `docs/archive/history/rc-tur-legacy-instances-do-not-compile.md` fix
direction 2: on the *parameter* side, `(t a)` instantiates to
`(type-app rc<?> tyvar 'a')` and does not unify with a `rc<A>` parameter either.
Both look like the same missing normalization of a `TY_APP` whose head is a
built-in pointer-family type constructor down to the concrete `TY_RC` form.

## Fix directions

1. Normalize `TY_APP(rc, X)` to `type_rc(kind of X)` (and the `weak`/`ref`
   equivalents) when instantiating an instance method's parameter and result
   types. Fixing both sides together is probably one change.
2. With that in place, `Foldable [rc]`'s bodies could drop their inline-C
   entirely and be written in pure Turmeric -- see the fix-direction-2 note in
   the archived report, which is blocked on exactly this.
3. Pin with a fixture that `rc/drop`s an `fmap` result and asserts the strong
   count of the original is untouched.
