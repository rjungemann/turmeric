# "Ambiguous method dispatch" error misleads when the real cause is a missing instance (and points at macros.tur for derive-emitted calls)

**Severity:** low-medium (confusing but *caught* compile error; the silent
sibling -- a segfault -- is tracked separately, see Related)

## Summary

When a `.method` call's receiver is a concrete type that has **no** instance of
that method's typeclass, and the receiver lowers to the `int64_t` carrier
(anything but a distinct scalar ABI -- `bool` here, opaque handles, structs),
`elab_method_call` reports a `TUR-E0020` *ambiguous* dispatch rather than a
*missing instance*.  Two things make it hard to act on:

1. **Misleading message.** It lists the program's *other* instances plus a
   phantom `Show[?]` as if they were candidates, and hints "annotate the
   receiver's type or use @TypeName syntax" -- but the receiver type is known
   and concrete; the true problem is simply "no `Show` instance for that type".
2. **Wrong file for macro-emitted calls.** `derive-show` / `derive-show-cstr`
   emit `(.show (.field __p))`, so the diagnostic's `call->span` is the
   macro body in `stdlib/macros.tur`, not the user's `derive-show` call or the
   offending struct field.

## Repro A -- via a derive macro (points at macros.tur)

```turmeric
(load "stdlib/str-build.tur")
(defclass Show [a] (show [x] : cstr))
(definstance Show [int] (show [x] : cstr (int->str x)))
(defstruct HasBool :copy [flag : bool n : int])   ;; no Show[bool] in scope
(derive-show-cstr HasBool flag n)
(defn main [] : int (let [h (make-struct HasBool true 5)] (do (println (.show h)) 0)))
```

```
stdlib/macros.tur:161:15: error [TUR-E0020]: ambiguous method dispatch: '.show'
matches 2 instances (Show[HasBool], Show[?]) -- receiver type is erased
(int64_t). Hint: annotate the receiver's type or use @TypeName syntax (see D1).
```

The user's mistake is "no `Show[bool]`", but nothing in the message says `bool`,
and it points at a stdlib file.

## Repro C -- direct `.show`, same message, correct file

```turmeric
;; ... same Show class + Show[int] + a Show[Foo] ...
(defn main [] : int (do (println (.show true)) 0))   ;; bool: no instance
```

```
q3.tur:6:34: error [TUR-E0020]: ambiguous method dispatch: '.show' matches 2
instances (Show[Foo], Show[?]) -- receiver type is erased (int64_t). ...
```

Direct calls at least point at the user's line, but the message is the same
misleading "ambiguous / Show[?] / annotate" wording.

## Root cause

`src/compiler/elab_typeclasses.c` ~5747 (`TUR_E0020_AMBIGUOUS_DISPATCH`): when
the carrier-erased receiver matches more than one carrier-compatible instance,
it enumerates every name-matching instance into the message.  An instance whose
`type_arg_syms[0]` is null renders as `Show[?]` (the phantom/carrier
representative).  The message uses `call->span`, which for a macro-emitted
`.show` is the macro body's span.

## Fix directions

- **Diagnose the real cause.** When the receiver's *static* type is a concrete
  type (not an unresolved tyvar) with no matching instance, emit
  "no instance of `Show` for `<receiver type>`" at that receiver, and drop the
  `Show[?]` phantom and the annotate/@TypeName hint (which apply to genuinely
  ambiguous erased-tyvar dispatch, not this case).  Reserve the current
  ambiguous wording for a true erased-tyvar receiver.
- **Attribute macro-emitted calls to the call site.** Mirror the orphan-check
  re-span fix (elab_call.c re-spans a macro-emitted top-level `definstance` to
  the call site): a `.method` call synthesized by a derive macro should carry
  the derive call-site span so the error names the user's struct/field, not
  `stdlib/macros.tur`.

## Related

The *silent* sibling of this bug is worse: the OWNED `derive-show` over a struct
whose field type lowers to the int64 carrier and has no `Show` instance
(e.g. `(defopaque Widget :int)` with no `Show[Widget]`) does **not** error --
its `(show (.field __p))` falls back to a carrier-compatible representative and
**segfaults at runtime**.  That silent carrier-fallback is
`docs/reported/method-dispatch-missing-instance-falls-back-to-carrier-representative.md`;
this report is the *caught*-but-confusing face of the same dispatch gap.  Both
would be resolved by grounding `.method` dispatch on the receiver's concrete
static type and erroring cleanly when no instance exists.
