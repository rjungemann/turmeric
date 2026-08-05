# `tur/` macros invisible to sibling stdlib files across interpreter re-elaboration

**Severity:** low (noisy warnings + latent runtime-dispatch fallback; no wrong
answers, since the affected macros still resolved at runtime). **Status:**
RESOLVED.

## Symptom

Every REPL turn after startup printed three spurious warnings, e.g. on the
first real expression:

```
turmeric> (+ 1 1)
stdlib/typeclass-show.tur:169:8: warning [TUR-W0040]: unknown name 'when'; will runtime-dispatch -- typo?
stdlib/typeclass-show.tur:197:8: warning [TUR-W0040]: unknown name 'when'; will runtime-dispatch -- typo?
stdlib/typeclass-show.tur:228:8: warning [TUR-W0040]: unknown name 'when'; will runtime-dispatch -- typo?
=> 2
```

Lines 169/197/228 of `stdlib/typeclass-show.tur` are the only three real
`(when ...)` call sites among the interpreter-preloaded stdlib files
(`vec-show-loop` / `set-show-loop` / `map-show-loop`). `when` is a `defmacro`
in `stdlib/macros.tur`, inside `(defmodule tur/macros ...)`.

Notably the warnings did **not** fire during the pure preload (an immediate
`:quit` was clean) -- only once a user turn followed.

## Root cause

`when` is defined in module `tur/macros`; `typeclass-show.tur` has no
`defmodule`, so it elaborates with `current_module_name == NULL`.
`elab_lookup_macro` (`src/compiler/elab_core.c`) only made a module-scoped
macro visible if its `defining_module_name` was `NULL`, equal to the current
module, `is_referred`, or on the macro-expansion stack -- so a `NULL`-module
file could not see a `tur/macros` macro.

The mechanism that is *supposed* to bridge this is the end-of-stdlib "M7"
promotion sweep in `src/compiler/elab_toplevel.c` (~line 1816), which rewrites
every `tur/`-module macro's `defining_module_name` to `NULL`. But that sweep
fires exactly **once**, at the stdlib/user boundary (`i + 1 == stdlib_prefix`).

The interpreter preloads stdlib incrementally via separate `turi_eval` calls
that **accumulate source** and **re-elaborate the whole combined program every
turn** (`src/turi/eval.c`, `env->src_acc` / `stdlib_prefix = prior`):

- On the preload turn that first loads `typeclass-show.tur`, that file is the
  *new tail* (after the boundary), so the sweep has already promoted `when`
  before it elaborates -> clean.
- On every **subsequent** turn, `typeclass-show.tur` sits *inside* the
  accumulated stdlib prefix and the user's new form is the tail. The single
  promotion sweep now lands at the boundary **after** `typeclass-show.tur`, so
  `when` is still `tur/macros`-scoped when `typeclass-show.tur` re-elaborates
  -> lookup misses -> eval-mode fallback emits TUR-W0040 and a runtime-dispatch
  call (which happens to still work, masking the defect as mere noise).

The compiled path never hit this because it does not auto-load
`typeclass-show.tur` at all (`show` is unresolved there without an explicit
load).

## Fix

Honour the documented "the `tur/` namespace is implicitly imported everywhere"
contract (see the `stdlib/macros.tur` header) directly at lookup time. In
`elab_lookup_macro` (`src/compiler/elab_core.c`), a macro whose
`defining_module_name` begins with `tur/` is now visible regardless of the
current module -- so visibility no longer depends on the promotion sweep having
already run. This is strictly consistent with the sweep, which promotes exactly
those macros to `NULL` for the whole program anyway; it just applies the same
rule earlier and unconditionally.

## Repro (pre-fix)

```
printf '(+ 1 1)\n:quit\n' | ASAN_OPTIONS=detect_leaks=0 build/tur repl
# -> three TUR-W0040 'unknown name when' warnings before `=> 2`
```

Post-fix the REPL prints `=> 2` with no warnings.
