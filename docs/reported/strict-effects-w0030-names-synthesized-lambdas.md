# TUR-W0030 names synthesized lambdas (`__fn_38`), which a user cannot act on

**Severity: low (diagnostic quality), blocking a default.** 47 of 358
`TUR-W0030` warnings over the fixture corpus name a compiler-synthesized
lambda rather than anything in the user's source, so the warning's own advice
-- "add `#{...}` to the function" -- names no function the user wrote. Filed
2026-10-01 while measuring whether `--strict-effects` could default on.

**Status: OPEN.** Not a wrong answer, and harmless while the flag is opt-in.
It is filed because it is the thing standing between `--strict-effects` and a
default: shipping 13% unactionable warnings in a default-on lint teaches
people to ignore the category.

## Minimal repro

Any `fn` literal that performs an effect its enclosing `defn` does not
declare. From the corpus (`stdlib/backtrack-dfs.tur`):

```sh
./build/tur --strict-effects check stdlib/backtrack-dfs.tur
```

```
stdlib/backtrack-dfs.tur:107:3: warning [TUR-W0030]: function '__fn_38'
  performs effects {Bt} but has no effect-row annotation
  (add #{...} or handle all effects inside the function)
```

There is no `__fn_38` in the source. The span is right -- it points at the
`fn` literal -- but the name is an elaborator-internal gensym, so the
message cannot be grepped, cannot be matched against the file, and reads as a
compiler-internals leak.

## Measured incidence

Over `tests/fixtures/` plus `stdlib/` (2750 files that check clean without the
flag), with `--strict-effects`:

| | count |
| --- | --- |
| files gaining at least one W0030 | 202 (7.3%) |
| W0030 warnings total | 358 |
| naming a synthesized lambda (`__fn_N`) | 47 |
| naming any compiler-internal `__` symbol | 50 |
| naming a real user-written function | 308 |

## Root cause

`src/passes/effect_check.c:1511-1526` emits the warning with
`fn->binding->name->name`, falling back to `"<anonymous>"` only when there is
no binding at all. A `fn` literal *does* get a binding -- a synthesized one --
so the fallback never fires and the gensym is printed instead. The
`--lint-effects` copy of the same message
(`effect_check.c:1558-1578`) has the identical bug.

## Fix directions

1. **Describe the lambda instead of naming it.** `"anonymous function at
   line 107"`, or `"the function passed to 'dfs-or'"` when the elaborator
   knows the call it is an argument to. Two call sites to change, both in
   `effect_check.c`.
2. **Suppress W0030 for synthesized bindings entirely**, and rely on the
   enclosing `defn`'s own warning. Note the corpus says this would hide
   genuine cases: in `backtrack-dfs.tur` the enclosing `defn` warns too, but
   that is not guaranteed in general.
3. **Mark synthesized bindings** with a flag the diagnostic layer can read,
   which fixes this class wherever else a gensym reaches a message rather than
   only here. Widest fix; worth a grep for other `binding->name->name` uses in
   diagnostics first.

Direction 1 is the cheap correct one. Direction 3 is the one that stops this
recurring.
