# RESOLVED: module-private defns collided on the bare name under `--interpret`

**Status:** Fixed 2026-06-12 (`src/turi/eval.c`, `src/turi/env.h`). Archived
resolution paper-trail for the `codegen-private-defn-collision` holdout tracked
in [turi-pure-turi-silent-miscompiles.md](turi-pure-turi-silent-miscompiles.md).

## Summary

Two modules that each defined a **private** (non-exported) helper with the same
name silently miscompiled under `--interpret`: both registered the helper under
the bare name in the single flat global environment, so the second definition
clobbered the first. Each module's exported function then called the *surviving*
helper instead of its own.

`codegen-private-defn-collision` exercises exactly this:

```turmeric
(defmodule alpha (export alpha-val)
  (defn __h [] : int 100)            ;; private
  (defn alpha-val [] : int (__h)))   ;; exported, calls alpha's __h
(defmodule beta (export beta-val)
  (defn __h [] : int 200)            ;; private, same bare name
  (defn beta-val [] : int (__h)))    ;; exported, calls beta's __h
```

- **Got:** `200 / 200` (both `__h` resolved to beta's, the last registered).
- **Expected:** `100 / 200`.

The compiled path is correct because it mangles each module's private defns to a
per-module C symbol. **Severity:** High (silent wrong answer), bounded to
`--interpret`.

## Root cause

`EX_FN_DEF` registered every top-level defn under `fndef->binding->name->name`
in the one global name->value map, and `EX_CALL` resolved call heads by that
same bare name (`eval_lookup` -> `turi_env_get`). Module privacy was never
modeled in the interpreter, so same-named privates across modules occupied one
slot (last-wins) and intra-module calls could not distinguish them.

## Fix

Mirror the compiled per-module mangling at the interpreter level:

1. **`TuriEnv`** (`env.h`) gains `defining_mod` (the `DefModule*` whose body is
   currently being evaluated) and `current_module` (the module owning the
   closure currently executing).
2. **`EX_DEFMODULE`** publishes `env->defining_mod` around its body eval
   (save/restore).
3. **`EX_FN_DEF`** consults `defining_mod`: a defn **not** in the module's
   `exports` list is *private*. Privates register under the qualified key
   `"<module>/<name>"`, and additionally under the bare name **only when that
   slot is still free** (first-wins) -- this keeps the entry-point `main` and
   legacy flat-namespace cross-module references reachable without letting a
   second module's same-named private clobber the first. Every closure (private
   or exported) is tagged with its owning module (`TuriClosure.module`).
4. **`eval_apply`** is split into an inner trampoline plus a thin wrapper; the
   inner loop publishes `env->current_module = cl->module` per tail-call
   iteration, and the wrapper saves/restores the caller's module across the
   whole call so a callee's module context never leaks back.
5. **`eval_lookup`** probes `"<current_module>/<name>"` before the flat global
   name, so a function running inside module M resolves M's own private helper.

## Validation

- `codegen-private-defn-collision` prints `100 / 200` under `--interpret`;
  added to the `tests/run-turi.sh` allowlist.
- `TUR=./build/tur bash tests/run-turi.sh` -> `980 passed, 0 failed`.
- Full fixture suite (`bash tests/run.sh`) -> `1596 passed, 0 failed`.
- `ctest -R tur_eval_import` (dedicated module/import runner) green.

## Notes / scope

- Non-`defn` module-private globals (`def`) are not yet qualified; the failing
  fixture only involves `defn`, and no other fixture exercises a private-`def`
  collision. Extend the same key scheme to `EX_DEF` if one appears.
- The bare-alias fallback intentionally preserves the interpreter's historical
  flat-namespace tolerance (a module with no own `name` can still reach another
  module's same-named private via the bare slot). Strict module-local
  enforcement would be a larger, separate behavior change.
