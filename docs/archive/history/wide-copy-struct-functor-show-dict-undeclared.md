---
title: "Wide `:copy`-struct functor + Show dispatch: emitted dict symbol undeclared"
category: Reported
description: A van-Laarhoven lens over a WIDE `:copy`-struct functor whose mapper
  dispatched a second constraint (`Show`) took the by-value monomorphization path
  (Path B), which specializes only the functor tyvar and emitted an undeclared
  dict binding for the Show dispatch, failing at cc. RESOLVED (2026-07-06) by
  deferring any lens carrying more than the lone `^Functor f` constraint to the
  dict-clone path (Path A), which threads every constraint dict.
---

# Wide `:copy`-struct functor + Show dispatch: emitted dict symbol undeclared

**Status:** RESOLVED (2026-07-06). A lens carrying a second constraint beyond
`^Functor f` now defers from the by-value mono (Path B) to the dict-clone (Path
A). See the Resolution section at the end.

**Summary:** A van-Laarhoven lens whose functor is a WIDE `:copy` struct (not the
one-int64 `defopaque` carrier) and whose mapper dispatches a second constraint
(`Show`) emits C that references an undeclared dict symbol
(`_un_undict_unNNNN_MMMM`), so `cc` fails. The `defopaque [a] :int` Identity form
of the same program compiles and runs correctly. **Severity: low** (narrow
shape; a hard compile error at `cc`, not a miscompile or silent wrong answer).

## Repro

```turmeric
(defstruct Identity :copy [a] (wrapped : a))          ; WIDE :copy struct functor
(defn mk-id  [A] [x : A]            : (Identity A) (make-struct Identity :wrapped x))
(defn run-id [A] [i : (Identity A)] : A            (.wrapped i))
(definstance Functor [Identity]
  (fmap [i g] (mk-id (g (run-id i)))))

(defclass Show [a] (show [x] : cstr))
(definstance Show [int] (show [x] : cstr (if (< x 0) "neg" "nonneg")))

(defn deep-lens [^f a] [^Functor f ^Show a g : (-> a (f a)) s : a] : (f cstr)
  (fmap (g s) (fn [x : a] : cstr (show x))))          ; depth-0 Show dispatch

(defn use-deep
    [l (forall [(f :: * -> *) a] [(Functor f) (Show a)] (-> (-> a (f a)) a (f cstr)))
     s : int] : cstr
  (run-id (l (fn [x : int] : (Identity int) (mk-id x)) s)))

(defn main [] : int (println (use-deep deep-lens 7)) 0)
```

```
tur run <file>
# /tmp/tur-build/....c: error: '_un_undict_unNNNN_MMMM' undeclared (first use in this function)
# tur: cc invocation failed
```

Swapping the functor to `(defopaque Identity [a] :int)` with inline-C
`mk-id`/`run-id` compiles and prints `nonneg`. The `--interpret` path runs the
`:copy`-struct form correctly (it never hits this codegen path).

## Notes

- Independent of the directly-applied-nested-lambda dispatch shape
  (`forall-dict-direct-applied-nested-lambda-dispatch`, now resolved): this
  fails even in the depth-0 workaround form above, where the mapper dispatches
  `Show` directly with no inner lambda.
- The undeclared symbol is a dict binding the clone's mapper env expects but the
  emitter never declares/defines for a wide `:copy` functor carrying a second
  constraint dict. Likely in the dict-clone / mapper-env emit for a by-value
  aggregate functor (contrast the int64-carrier `defopaque` path, which names
  the dict correctly).
- Compiled fixtures in `tests/fixtures/van-laarhoven-lens-show-*` all use the
  `defopaque [a] :int` Identity, so none exercises the wide `:copy`-struct
  functor with a Show constraint -- this shape is uncovered.

## Fix directions

Trace where the mapper-env dict symbol (`_un_undict_...`, the mangled dict
binding) is referenced vs declared in the emitter for a `:copy`-struct functor
result, and emit the declaration/definition (or thread the existing dict) on the
by-value-aggregate path as the int64-carrier path already does.

---

## Resolution (2026-07-06)

The root cause was a path collision, not a missing declaration. A WIDE functor
routes `deep-lens` through the van-Laarhoven by-value monomorphization (Path B,
`vl-wide-mono`), which emits `deep_lens__mono_<hash>(g, s)` -- specializing ONLY
the functor tyvar `f := Identity` and leaving the element tyvar `a` abstract. But
the forall-dict-pass had already lowered the mapper `(fn [x] (show x))` to a
dict-capturing closure expecting the `^Show a` dict as a dict-clone PARAMETER
(`__dict_1296`). Path A (the dict-clone `deep_hylens_...(__dict_1294, __dict_1296,
g, s)`) supplies that parameter and is emitted correctly; Path B's mono body has
no such parameter, so building the mapper env emitted
`__t62->__dict_1296 = <undeclared __dict_1296>` and `cc` failed. (The narrow
`defopaque` functor is not wide, so it never took Path B -- hence it worked.)

Path B fundamentally cannot resolve a constraint it does not monomorphize: it
binds only `f`, so a `^Show a` dispatch on the still-abstract `a` genuinely needs
a runtime dict Path B has no slot for.

**Change** (`src/compiler/mono_specs.c`, `lens_is_simple_for_pathb`): a van
Laarhoven lens always carries `^Functor f`; the gate now additionally requires
`lens->constraints.n_constraints <= 1`, so a lens with ANY extra constraint is
NOT eligible for Path B and falls back to Path A. Path A threads every constraint
dict (by value / int64 carrier) and is already correct -- it merely boxes the
wide functor at the lens crossing instead of passing it zero-overhead by value.
For this narrow shape that is the right trade: correctness over an optimization
that cannot apply.

**Verified.** The repro compiles and prints `nonneg`; a variant over both
`Show int` and `Show bool` (7 -> `nonneg`, -5 -> `neg`, `true`/`false`) confirms
the threaded dict dispatches per instance. `deep_lens__mono` is no longer emitted
for this lens; the dict-clone is used and every dict reference is a declared
parameter. New fixture `tests/fixtures/van-laarhoven-lens-wide-functor-show/`
covers the shape (no inline-C, so it runs under BOTH the compiled and interpreter
harnesses). `bash tests/run.sh`: 1951 passed, 0 failed (existing single-
constraint wide-mono fixtures, all `n_constraints == 1`, still take Path B
unchanged). `bash tests/run-turi.sh`: green.

**Not pursued:** teaching Path B to thread extra constraint dicts as parameters
(or to also monomorphize on `a` when concrete) so a wide functor with a second
constraint gets the zero-overhead by-value treatment. That is a real
optimization extension to the mono subsystem, worth doing only if this shape
becomes hot; deferring to Path A costs one carrier box/copy at the crossing,
exactly as any wide functor did before `vl-wide-mono` graduated.
