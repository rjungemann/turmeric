# Dot-method dispatch on a receiver with no instance silently falls back to a carrier representative

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
