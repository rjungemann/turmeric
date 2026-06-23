# Auto-loaded `defmodule tur/*` macros are not promoted to global visibility

**Severity:** low (workaround available)

## Summary

When a stdlib file in `stdlib_files[]` (src/main.c, the compiled-path auto-load
list) wraps its contents in `(defmodule tur/<name> ...)`, the module's **defns**
become globally visible from user code (no import needed) but its **defmacros**
do not -- a call to such a macro is elaborated as an ordinary function call.

## Minimal repro

`stdlib/unique.tur` (auto-loaded under `-Xunique-types`) wrapped as:

```turmeric
(defmodule tur/unique
  (export with-unique consume)
  (defmacro with-unique [binding & body]
    (let [bname (first binding) binit (second binding)]
      (list let `[^unique ~bname ~binit] (list do ~@body))))
  (defn consume [A B] [^unique x : A f : (fn [A] B)] : B (f x)))
```

User program:

```turmeric
(defn dbl [n : int] : int (* n 2))
(defn main [] : int
  (with-unique [u 21] (println (consume u dbl)))   ;; consume resolves; with-unique does not
  0)
```

`tur build -Xunique-types` reports:

```
error: phase 1: vector literals are only allowed in let bindings
  (with-unique [u 21] ...)
               ^^^^^^
```

i.e. `with-unique` was treated as a function call (its `[u 21]` argument hit the
vector-literal lowering), while `consume` from the same module resolved fine.

## Root cause (direction)

`src/compiler/elab_toplevel.c` ~line 1229 promotes auto-loaded `tur/*` module
members back to "stdlib pre-module" (`defining_module_name = NULL`, globally
visible) once the stdlib prefix finishes. The binding loop (~1230) promotes
globals; the macro loop (~1239) is supposed to do the same for `e->macros`, but
the macro defined inside the auto-loaded `defmodule` is still looked up as a
plain call at the user site (`elab_lookup_macro`, elab_core.c:1707, only returns
a macro when `defining_module_name` is NULL / current-module / on the expansion
stack). The defn promotion works but the macro promotion does not take effect
for the auto-loaded `defmodule` case -- timing of registration vs. the promotion
sweep, or the macro never reaching the promoted state.

## Workaround (in tree)

`stdlib/unique.tur` ships as a **bare** definition file (no `defmodule`
wrapper), the same convention as `stdlib/math.tur` / `stdlib/bits.tur`. Bare
top-level macros are pre-module (`defining_module_name == NULL`) and are always
visible, so `with-unique` expands correctly.

## Fix directions

Either (a) ensure the `tur/*` macro-promotion sweep runs after every
auto-loaded `defmodule` macro is registered (so `defmodule`-wrapped stdlib
macros match the always-visible behavior of `tur/macros`), or (b) document that
auto-loaded stdlib macro modules must be bare files.
