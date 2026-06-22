# U5 regex matcher-as-cata: two follow-up codegen edges past #499

**Found by:** turmeric-spices Track C (U5 regex matcher refactor attempt
after #499 landed)
**Verified on:** turmeric main @ `affeb7a6` (post #499), Debug ASan build
**Severity:** Low-Medium. Blocks the spice-side matcher-as-cata landing
in `turmeric-spices/spices/regex/src/regex/tree.tur`. `re-matches?` remains
direct structural recursion. Value folds (`re-size`, `re-nullable?`,
`re->str`) and the matcher itself are unaffected.

## Context

#499 fixed the cata-application-site case where the carrier `B` has a
function-typed argument (`B = (fn [(fn [cstr] bool) cstr] bool)`). With
that fix, a minimal `cata` whose carrier takes a function-typed argument
runs end to end.

Attempting to use the now-fixed carrier shape to express the regex
backtracking matcher as `(re-cata match-alg e)` -- i.e. swap the existing
recursive `m` for an F-algebra over the matcher closure carrier -- the
codegen hits two further edges that #499 did not cover.

## Edge 1 -- `letrec`-self inside a returned nested closure in a match arm

The natural `StarF` arm wants a local `letrec` whose closure references
itself and is returned out of the algebra:

```turmeric
(StarF x) (letrec [self (fn [k : (fn [cstr] bool) s : cstr] : bool
                          (or (k s)
                              (x (fn [s2 : cstr] : bool
                                   (if (cstr-same? s2 s) false (self k s2)))
                                 s)))]
            self)
```

Emits a body that references `self_1149` from inside a nested closure
without including it in the closure's captures:

```c
__t25 = regex__tree____fn_1158((void *)(intptr_t)(self_1149),
                                (int64_t)(intptr_t)(__env___env_1155->k),
                                s2);
// error: use of undeclared identifier 'self_1149'
```

The outer letrec-bound `self` is reachable from the inner `(fn [s2] ...)`
closure body, but `collect_free_vars` is not picking it up across that
inner-closure boundary.

## Edge 2 -- env-struct redefinition collision when the letrec is hoisted

Workaround attempt: hoist the StarF loop to a top-level `defn` that takes
the folded child matcher `mx` explicitly:

```turmeric
(defn star-loop [mx : (fn [(fn [cstr] bool) cstr] bool)
                 k  : (fn [cstr] bool)
                 s  : cstr] : bool
  (or (k s)
      (mx (fn [s2 : cstr] : bool
            (if (cstr-same? s2 s) false (star-loop mx k s2)))
          s)))
```

`tur check` passes; `cc` reports two distinct problems on the emitted C:

1. **Wrong dispatch signature for the fn-typed parameter `mx`** at its
   call site -- the args list collapses to `(size_t, const char *)`:

   ```c
   __t36 = ((bool (*)(size_t, const char *))(intptr_t)mx)(__t38, s);
   // error: incompatible pointer to integer conversion passing 'void *'
   //        to parameter of type 'size_t'
   ```

   `mx` is `(fn [(fn [cstr] bool) cstr] bool)`; the dispatch should be
   `bool (*)(void* env, int64_t, int64_t)`. The first parameter (which is
   itself a function) is being demoted, and the second parameter (`cstr`)
   is being narrowed too.

2. **Spec collision on `__env_1068__spec__bool`** -- two distinct
   definitions emitted in the same TU, one with a thin
   `tur_thunk_bool_int64_t_t __fn` field, one with `int64_t __fn`:

   ```c
   // first definition
   struct __env_1068__spec__bool { tur_thunk_bool_int64_t_t __fn; int64_t alg; };
   // ...
   // second definition (redefinition error)
   struct __env_1068__spec__bool { int64_t __fn; int64_t alg; };
   ```

   Looks like the same closure environment is being specialized twice with
   different ABIs for the embedded `__fn` slot (thin vs. fat), and both
   emit at the same C name.

## Minimal repro

Apply the matcher-as-cata patch to `spices/regex/src/regex/tree.tur`
(replace the existing direct-recursion `m` with an algebra over a function
carrier as sketched in
`turmeric-spices/docs/u5-regex-matcher-cata-blocker-2026-06-22.md`). Run
`tur run spices/regex/tests/tree_test.tur`. Both edges fire in the same
emit.

## Fix direction

- **Edge 1:** extend `collect_free_vars` so the letrec-bound name is
  captured by an inner closure that references it from inside its body.
  Same shape as #494/#496-class capture fixes but for `letrec` self
  through one further closure layer.
- **Edge 2:** at the closure-spec mangler, ensure a function-typed
  parameter materializes the env struct's `__fn` field as the fat thunk
  type (`tur_thunk_*`), not as `int64_t`, when the same env layout is
  reachable via both a fat and a thin code path. The redefinition is a
  symptom -- the underlying issue is a missing thin/fat consolidation
  before naming.

Until both edges are addressed, the regex matcher stays direct structural
recursion (currently 14/14 green at
`turmeric-spices/spices/regex/tests/tree_test.tur`).
