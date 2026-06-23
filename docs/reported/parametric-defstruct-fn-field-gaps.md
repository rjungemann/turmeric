---
status: open
severity: medium
discovered: 2026-06-23
discovered-by: investigating polymorphic `Lens' S A` example
---

# Parametric defstruct + fn-typed fields: four gaps blocking `Lens' S A`

## Summary

Building a polymorphic `Lens' S A` (the canonical lens-prime: a struct of
`get : S -> A` and `put : S -> A -> S`) currently fails on four distinct
issues. Multi-param parametric `defstruct` itself works (`Pair [A B]`,
`Result [A B]`, `Tuple6 [A B C D E F]`), and fn-typed fields work on
non-parametric structs with primitive args (`fn-field-unboxed` fixture).
The combination — and even some monomorphic cases — does not.

The only Lens-shaped struct that runs end-to-end today is the all-`int`
form (verified working at `/tmp/lens-demo/lens-int.tur`).

## Gap 1 — single-arg fn-typed field mis-parses as 1-tuple

Inside any `defstruct` field, `(fn [T] U)` triggers:

```
error: tuple type `[...]` must have 2 to 8 element types (got 1);
use (TupleN ...) or a named struct instead
```

`T` can be a type var or a concrete primitive — the bracket is being
parsed as a TupleN literal rather than as a fn arg-vector. The same
`(fn [T] U)` works fine in `defn` parameter type positions
(`stdlib/parsec.tur:337`, `stdlib/typeclass-bifunctor.tur:13`), so the
defstruct field-type parser is on a different path.

### Repro

```turmeric
(defstruct Box [A] (run (fn [A] A)))
(defn main [] : int 0)
```

```
$ ./build/tur check repro.tur
repro.tur:1:24: error: tuple type `[...]` must have 2 to 8 element types (got 1)
```

### Suspected location

`src/compiler/elab_structs.c` — the field-type elaboration path needs to
recognize `(fn [args] ret)` before falling through to TupleN. Worth
diffing against the `defn` arg-type parser to see what dispatch the
field path is missing.

## Gap 2 — struct's type params not in scope inside fn-typed fields

With ≥2 fn args (sidestepping Gap 1), the struct's `[S A]` parameters
aren't bound when the field's fn type is kind-checked:

```turmeric
(defstruct Lens [S A]
  (op (fn [S A] S)))
```

```
error [TUR-E0012]: kind mismatch: cannot apply a type of kind '*'
as a type constructor; expected an arrow kind (* -> * or higher)
```

This is a scope/binding bug — `[S A]` is correctly added as the struct's
kind signature (Gap 1's probe shows `Box [A]` parses fine when the field
is a plain `A`), but the binding doesn't extend through the fn type
inside a field.

## Gap 3 — parametric struct return value won't unify at call site

Even with a plain (non-fn) parametric struct:

```turmeric
(defstruct Box [A] (val A))
(defn make-box [A] [a : A] : (Box A) (make-struct Box a))
(defn unbox    [A] [b : (Box A)] : A (.val b))
(defn main [] : int (println (unbox (make-box 42))) 0)
```

```
error [TUR-E0001]: function 'unbox' arg 1:
expected ptr<void>, got (type-app Box int)
```

`make-box 42` produces `(type-app Box int)` but the polymorphic `(Box A)`
parameter slot doesn't unify with it. Either the type-app isn't being
constructed in a form unifier recognizes, or `unbox`'s `A` is being
defaulted to `ptr<void>` instead of left as a tyvar awaiting unification.

## Gap 4 — codegen drops the fn signature when fn fields mention struct/cstr types

Monomorphic Lens elaborates but C compilation fails:

```turmeric
(defstruct Person :copy [name : cstr age : int])
(defstruct Lens :copy
  [get (fn [Person] cstr)
   put (fn [Person cstr] Person)])
```

```
/tmp/tur-build/.../lens-mono_tur.c:5541:
  error: incompatible integer to pointer conversion initializing
  'const char *(*)(int64_t)' with an expression of type 'int64_t'
    const char * (*__call_head_1255)(int64_t) = (l).get;
```

The field is declared `int64_t` in the emitted struct, but the call site
casts to the typed C function pointer with no `(intptr_t)` bridge. The
existing `fn-field-unboxed` fixture only exercises primitive int args
(`fn [int32] int32`); anything involving structs or `cstr` falls off
that path. Either the field needs the typed `tur_fnptr_..._t` codegen
the unboxed path uses for primitives, or the call site needs the
`(intptr_t)` cast its boxed-int sibling uses elsewhere.

## Cheap-wins recommendation

Gaps 1 and 4 look like the smallest fixes and unblock real ergonomics:

- **Gap 1** is a parser dispatch fix — recognize `(fn ...)` before the
  TupleN fall-through in the field-type path.
- **Gap 4** is either a codegen extension (emit `tur_fnptr_..._t` for
  any concrete fn signature, not just all-primitive) or a one-line
  `(intptr_t)` insertion at the call site.

Gaps 2 and 3 are deeper (parametric-struct type-var scoping and
type-app unification at polymorphic call sites) and probably want to
ride along with broader monomorphization work — see
`[[project_monomorphization_north_star]]`.

## Workaround today

`/tmp/lens-demo/lens-int.tur` — Lens over `int`, `(fn [int] int)`
fields, all-`:copy`. Works and runs. Anything with structs, strings,
or polymorphism currently does not.
