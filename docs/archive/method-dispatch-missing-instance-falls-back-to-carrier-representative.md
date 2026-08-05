# Dot-method dispatch on a receiver with no instance silently falls back to a carrier representative

**Status: RESOLVED (2026-07-21).** Fixed in `src/compiler/elab_typeclasses.c`
alongside its caught-but-confusing sibling
(`ambiguous-dispatch-error-quality-missing-instance.md`); full suite 2247 passed,
0 failed.

The dispatch fallback loop recorded a type-mismatched instance as a fallback and
set `best_method` to the FIRST such fallback; when the receiver had no exact
match and exactly one carrier-compatible instance existed (`fallback_count == 1`),
that single representative was bound **silently** -- the wrong-instance dispatch
this report describes (a SIGSEGV when its layout differs, a silent wrong result
when it happens to match, as the `Widget`/`Show[int]` case shows).

The fix grounds the diagnosis on the receiver's concrete static type. Before the
`TUR_E0020` ambiguity check, when the receiver is a genuinely DISTINCT concrete
type -- a positive whitelist: `bool`/`cstr`/`float`/`float32`/`nil`/`sym`, the
sized ints, or a `TY_ADT` with a real def (structs, **opaque newtypes**, user
ADTs) -- and NO instance matches it exactly, dispatch now emits a clean
`TUR-E0015` *"no instance of typeclass '<Class>' for type '<type>'. Add
(definstance <Class> [<type>] ...) ..."* regardless of `fallback_count`, instead
of binding a representative. This is exactly the discriminator the "Root cause &
why it is not a trivial fix" section below proposes:

- **Concrete receiver, genuinely no instance** -> `TUR-E0015`. Verified for both
  the struct case (repro above: `.debug` on `Inner` -> "no instance of Debug for
  Inner" at the user's `derive-debug` line) and the carrier-collision case
  (`(:: 5 Widget)` with only `Show[int]` in scope -> "no instance of Show for
  Widget", previously silent).
- **Carrier-typed tyvar receiver in a polymorphic body** -> representative is
  still correct: excluded by `obj_is_abstract_tyvar` and by the whitelist (a bare
  `:int` carrier and null-def ADTs are excluded), so `vec-get`-style erased
  element dispatch and constrained-generic base clones re-specialize downstream
  exactly as before.
- **Recursive self-type** (a `Debug[Tree]` body calling `.debug` on a `Tree`
  subfield) -> an EXACT match sets `exact_match_found` and never reaches the new
  branch, so it still resolves to self.

Regression fixtures:
`tests/fixtures/errors/dispatch-opaque-receiver-no-instance/` (this report, the
silent single-representative case) and
`tests/fixtures/errors/dispatch-concrete-receiver-no-instance/` (the sibling
report). The `generic-show-dispatch-opaque` and `derive-debug-display` fixtures
(legitimate carrier/exact-match dispatch) still pass.

---

**Severity:** medium (silent wrong-instance dispatch when the receiver shares the
int64 carrier; a confusing generated-C type error otherwise)

## Background

This was originally filed as "derive-debug/display miscompile on non-int/ptr
fields." That had two compounding causes; the first is now fixed and this report
is narrowed to the second (deeper) one.

**FIXED (2026-07-20): missing `Debug`/`Display` primitive instances.**
`stdlib/typeclass.tur` now defines `Display` and `Debug` for `cstr`, `bool`, and
the full sized-numeric set (`Display` delegates to the matching `Show` body;
`Debug` keeps the `type(value)` tag the existing `Debug[int]`/`[ptr<void>]` use,
with `cstr` quoted and `bool` bare). `derive-debug` / `derive-display` over a
struct with cstr/bool/float fields now compile and render. Regression fixture:
`tests/fixtures/derive-debug-display/`.

## Remaining defect

A dotted `(.method recv ...)` call whose receiver's *concrete* type has **no**
instance of the method's typeclass does not error. Instead, `elab_method_call`'s
carrier-representative fallback (src/compiler/elab_typeclasses.c, the
"carrier-compatible representative instance" search, ~line 5349) selects some
carrier-compatible instance in scope -- in a derive body, the enclosing/self
instance being defined -- and dispatches there.

## Repro

```turmeric
(load "stdlib/typeclass.tur")
(load "stdlib/str.tur")
(defstruct Inner [k : int])          ;; NO Debug instance for Inner
(defstruct Outer [inner : Inner])
(derive-debug Outer inner)           ;; (.debug (.inner o)) has no Debug[Inner]
(defn main [] : int
  (let [o (make-struct Outer (make-struct Inner 5))]
    (do (println (debug o)) 0)))
```

`.debug` on the `Inner` field resolves to `__inst_Debug_debug_Outer` (the self
instance) instead of erroring:

```c
static const char * __inst_Debug_debug_Outer(tur_adt_Outer __p) {
    __auto_type __ps = (__inst_Debug_debug_Outer((tur_adt_Inner)(__p).inner)); /* wrong */
```

Here the C compiler catches it (`incompatible type`), because `Inner` and
`Outer` are distinct C aggregates. When the instance-less receiver instead
shares the **int64 carrier** with the fallback instance (an opaque newtype, a
:heap ADT, ...), the C types match and the wrong instance is called silently --
the same carrier-collision class as the (now-fixed) generic-`^Show a` dispatch
bug (`docs/archive/history/generic-show-dispatch-opaque-carrier.md`).

## Root cause & why it is not a trivial fix

The carrier-representative fallback is **intentional** for polymorphic dispatch:
a generic body dispatching a class method on a carrier-typed tyvar
(`(show (:: (vec-get v i) A))`) picks the int representative at elaboration, and
the ABI specializer later re-dispatches per concrete element. The fallback
cannot be turned into a hard error unconditionally without breaking that path.

The fix must distinguish:
- **concrete receiver, genuinely no instance** -> should be a clean
  `TUR-E00xx: no instance of Debug for Inner` at the call site; versus
- **carrier-typed tyvar receiver in a polymorphic body** -> representative is
  correct and gets specialized downstream.

A candidate discriminator: only take the carrier representative when the
receiver's static type is an unresolved/erased tyvar or a carrier-erased element
read (`obj_is_unascribed_carrier_elem`, already used nearby); a receiver with a
fully concrete nominal type (`Inner`, `cstr`, ...) and no matching instance
should fall through to the existing "no instance" diagnostic (~line 4930) rather
than the representative search. Needs care: the derive bodies feed concrete
field types, so the recursive self-type case (a `Debug[Tree]` body calling
`.debug` on a `Tree` subfield) must still resolve to self.

## Notes

`derive-debug`/`derive-display` had no fixture coverage before this, which is why
both this and the missing-instance gap went unnoticed.
