# A direct call to a constrained generic does not inherit the caller's tyvar binding

**Severity: medium.** A **silent wrong answer**, for a call that is arguably
ambiguous to begin with -- see "Is it even well-formed". What makes it worth
recording is that the compiled path now answers the SAME question two different
ways depending on whether the generic is called or passed.

**Status:** open. Found 2026-09-12 narrowing the turi fn-value case; the
direct-call half is pre-existing.

## Repro

```turmeric
(defn vjoin [V] [(JS V)] [x : int y : int] : int      ;; V is in NO parameter
  (:: (j (:: x V) (:: y V)) int))

(defn fold [f : (fn [int int] int) a : int b : int] : int (f a b))

(defn viadirect [V] [(JS V)] [a : (M V) b : (M V)] : int
  (vjoin (.e a) (.e b)))                              ;; called
(defn viafold [V] [(JS V)] [a : (M V) b : (M V)] : int
  (fold vjoin (.e a) (.e b)))                         ;; passed as a value
```

```
compiled:  viadirect Gmax => 9   viadirect Gsum => 9    <- WRONG, both Gmax
           viafold   Gmax => 9   viafold   Gsum => 12   <- correct
```

`viadirect` gets no specialization at all (`static int64_t viadirect(int64_t,
int64_t)` in the emitted C); `viafold` gets one per instantiation.

## Why the two differ

`vjoin`'s `V` appears in no parameter, so the CALL site can derive no binding
for it -- `(vjoin (.e a) (.e b))` passes two `int`s and pins nothing. The relay
probe that specializes a constrained caller requires some type argument to have
become concrete, and here none has, so it does not fire.

The fn-value path takes a different route: it hands the ENCLOSING
specialization's bindings to the clone directly, rather than composing the
call's own. So it resolves `V` to the caller's, and the direct call does not.

## Is it even well-formed?

Debatable, and worth settling before fixing. `vjoin`'s `V` and `viadirect`'s `V`
are independent type variables that merely share a spelling; nothing in the call
connects them. A Haskell-like reading makes the call **ambiguous** and demands
an annotation.

But the two paths must not disagree. Either:

- **inheriting is right** -- the same-named tyvar of an enclosing constrained
  generic is the intended one (this is what makes the `crdt/ormap` idiom work,
  where `__vjoin` is passed to a fold and must mean the map's value type), and
  the direct call should inherit too; or
- **inheriting is wrong** -- the call is ambiguous and should be a type error,
  in which case the fn-value path is over-resolving and the diagnostic belongs
  at both sites.

The first is the more useful reading and the one the fn-value path already
implements.

## Not this

The interpreter answers `12 12 12 12` here -- both shapes wrong -- for the
separate reason recorded in
[turi-nested-class-method-call-picks-first-instance](turi-nested-class-method-call-picks-first-instance.md).
Its fn-value half is fixed; its direct-call half tracks THIS report, since there
is no binding to capture in the first place.

## Fixture owed

The repro above, once the reading above is settled -- asserting either `9 12 9
12` or a diagnostic at both call sites.
