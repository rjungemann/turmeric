# Struct-of-functions lens composition — two downstream codegen blockers

**Severity:** medium (front-end now accepts the program; codegen still can't
lower it). Blocks the plain `:copy` struct-of-functions lens idiom end to end.

## Context

The Trowel `lens-example.tur` (a plain `defstruct Lens' :copy [S A]` holding
`get`/`put` closures, plus a generic `compose-lens`) first tripped a spurious
`TUR-E0005` use-after-move in the front end. That was a move-checker asymmetry
and is **fixed** (enclosing-signature tyvar params in an inner closure now get
`CK_COPY`, matching locally-declared type params — see
`src/compiler/elab_types.c` sig-tyvar recovery block, fixture
`enclosing-tyvar-closure-param-copy/`).

With the front end passing, the program now reaches codegen and hits two
*separate, pre-existing* bugs. A single generic struct-of-functions
instantiated at concrete types codegens and runs fine; both blockers below are
specific to a **generic function that builds** such a struct.

## Blocker 1 — struct name apostrophe leaks into C identifiers

The struct is named `Lens'`. The monomorph typedef/field emission puts the `'`
straight into the C identifier:

```
struct tur_adt_Lens'__Company__Person { ... }   // invalid C — bare apostrophe
```

`cc` rejects it. Non-`[A-Za-z0-9_]` characters in a Turmeric type name must be
mangled before they reach an emitted C identifier. (Renaming `Lens'` -> `Lens`
sidesteps this and exposes Blocker 2.)

## Blocker 2 — composed-generic-struct monomorph carrier mismatch

With the prime removed, codegen still fails building `compose-lens`:

```
error: assigning to 'int64_t' from incompatible type 'tur_adt_Lens__Person__cstr'
    __t208->l2 = l2;
```

`compose-lens` captures the two argument lenses (`l1`/`l2`) into the closure
environments of the `get`/`put` it builds. The captured field is emitted with
the generic `int64_t` carrier type while the value handed in is the concrete
`tur_adt_Lens__Person__cstr` monomorph struct — a by-value-vs-carrier mismatch
in the closure-environment struct for a generic function returning a
struct-of-closures. This is squarely the end-to-end-monomorphization territory
tracked by `docs/archive/history/van-laarhoven-*` and the monomorphization
north-star plan; the plain struct-of-functions lens is a distinct surface from
the Functor-encoded van Laarhoven path that was previously resolved.

## Minimal repros

Blocker 1: any `defstruct` whose name contains `'`, used as a type argument in a
monomorphized position.

Blocker 2 (prime removed):

```turmeric
(defstruct Lens :copy [S A] (get (fn [S] A)) (put (fn [S A] S)))
(defn compose-lens [S A B] [l1 : (Lens S A) l2 : (Lens A B)] : (Lens S B)
  (make-struct Lens
    (fn [s : S] : B ((. l2 get) ((. l1 get) s)))
    (fn [s : S b : B] : S ((. l1 put) s ((. l2 put) ((. l1 get) s) b)))))
```

A single generic struct-of-functions instantiated at concrete types (no
generic builder) codegens fine — so the trigger is specifically a generic fn
capturing struct values into the closures it constructs.
