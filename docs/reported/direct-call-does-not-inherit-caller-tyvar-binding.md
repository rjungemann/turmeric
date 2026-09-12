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

## Is it even well-formed? -- SETTLED 2026-09-12

The open question was whether the callee's tyvar should inherit the caller's
binding at all, given the two are independent variables that merely share a
spelling. Settling it turned up something worse, and it is worth recording
because the experiment is one line:

**Alpha-rename the callee's type parameter.** `[V]` -> `[W]`, changing nothing
else:

```
matching names  (callee [V]):  9 12   correct
alpha-renamed   (callee [W]):  12 12  WRONG -- both instantiations collapse
```

Both back ends. So the inheritance the fn-value path implemented was **name
capture**, not resolution: a generic's meaning depended on the SPELLING of a
bound type variable. `crdt/ormap` was one rename away from silently merging
every map with the wrong join -- verified by doing it, which turned
`test_ormap_join` red.

That settles the question against pure name matching, and in favor of the
**constraint's CLASS** as the key -- which is what a dictionary is keyed on
anyway: the caller holds a dictionary for class C, the callee needs one, pass
it. A caller with two constraints on one class (`[^Show K ^Show V]`) is
genuinely ambiguous and is skipped rather than guessed.

**Both paths are now class-keyed** (`emit_translate_bindings_by_class` in the
emitter; `frame_lookup_dict` in `src/turi/eval.c`'s `EX_VAR` capture), and
`tests/fixtures/constrained-generic-as-fn-value` carries alpha-renamed rows on
both back ends so it cannot regress to name matching.

## Not this

The interpreter answers `12 12 12 12` here -- both shapes wrong -- for the
separate reason recorded in
[turi-nested-class-method-call-picks-first-instance](turi-nested-class-method-call-picks-first-instance.md).
Its fn-value half is fixed; its direct-call half tracks THIS report, since there
is no binding to capture in the first place.

## What remains open

Only the DIRECT-call half. With the class as the key, the principled answer is
that `(vjoin (.e a) (.e b))` inside a constrained caller should resolve `vjoin`'s
constraint from the caller's dictionary for the same class -- exactly as the
fn-value path now does. It still does not: the call site derives no binding (its
tyvar reaches no parameter), so the relay probe never fires and `viadirect` gets
no specialization.

The two paths therefore still disagree, just on a sounder basis than before.

## Fixture owed

The repro above asserting `9 12 9 12`, plus an alpha-renamed sibling, once the
direct-call half resolves through the class the way the fn-value half does.
