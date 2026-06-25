# Untyped Native Registration Blocks Curated Facades Over Embedder Natives

> **Status:** Reported, not yet fixed
> **Severity:** Medium -- blocks ergonomic typed wrappers over embedder
> natives; existing scripts still work via direct calls.
> **Discovered:** 2026-06-25
> **Discovered by:** turmeric-godot G3.b prelude work (see
> `../../turmeric-godot/src/bridge/prelude.cpp`)

---

## Summary

`turi_env_register_native` and `turi_register_default_native` accept only
`(name, fn, ud)` -- no type signature. The elaborator therefore defaults
every interpreter-mode native to `dispatch_result = TYPE_INT` at
`src/compiler/elab_call.c:2386`, with a hand-rolled two-entry allow-list
for `error?` and `error-message`. Any embedder native whose runtime
result is `:float`, `:cstr`, `:bool`, or an opaque type is invisible to
the type system -- the elaborator believes the call returns `:int`.

The runtime is fine (the TuriValue carries its own tag), but a defn that
*wraps* such a native and declares the honest return type fails
elaboration with TUR-E0707 / TUR-E0708 because the wrapper's body is
typed `:int` even though the runtime value is a float / cstr.

This blocks a clean curated facade over any embedder native that returns
something other than `:int` -- exactly the shape every GDExtension /
spice ergonomics layer needs.

---

## Minimal repro

In the turmeric-godot embedder (any similar embedder reproduces it):

```c
/* embedder C: register a native that returns a float */
static TuriValue my_get_x(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)env; (void)args; (void)n; (void)ud;
    return turi_float(3.5);
}
turi_register_default_native("my-get-x", my_get_x, NULL);
```

In a script evaluated by that env:

```turmeric
(defn wrap-x [] : float (my-get-x))
;; TUR-E0707: function 'wrap-x' declares return type 'float'
;; but its body returns int -- a float and a non-float live in
;; different register classes ...
```

At runtime, `(my-get-x)` returns a TURI_FLOAT-tagged value; the elaborator
just doesn't know that at compile time.

`(my-get-x)` called directly works, because the elaborator types the
call expression as `:int` and the runtime value flows through that slot
without an actual int decode. The problem is purely at the boundary
where a wrapper's declared type is checked against the body's elaborated
type.

Witnessed today in turmeric-godot at
`src/bridge/prelude.cpp:14-26` (the comment block explains why the
prelude is constrained to void-returning setters and opaque-handle
queries).

---

## Root cause

**`src/compiler/elab_call.c:2376-2400` -- the eval-mode fallback for
unknown names.**

```c
/* eval mode: create a runtime-dispatch call so native builtins ... */
Type dispatch_result = TYPE_INT;
const char *nm = name->name;
if (nm) {
    if      (strcmp(nm, "error?") == 0)        dispatch_result = TYPE_BOOL;
    else if (strcmp(nm, "error-message") == 0) dispatch_result = TYPE_CSTR;
}
Binding *dyn_b = binding_new(e, name, dispatch_result, false, false, head->span);
```

The allow-list is the only mechanism for an interpreter-mode native to
present a non-`:int` return type. Argument types are not checked at all
here -- there is no signature for the elaborator to consult.

Supporting code:

- `src/turi/eval.c:142-150` -- `turi_env_register_native` writes a
  `TuriClosure` with only `{fn, captured, native, native_ud}` fields; no
  type information is captured.
- `src/turi/env.c:160-198` -- `turi_register_default_native` likewise
  takes only `(name, fn, ud)` and replays them at env-creation time.
- `src/turi/value.h:25-44` -- `TuriClosure` has no type-signature field.

---

## Fix directions

### A. Typed native registration API (preferred long-term)

Extend the registration API to carry a parsed type signature:

```c
/* New entry point; old one becomes a thin (TYPE_INT, TYPE_INT, ...) wrapper. */
void turi_env_register_native_typed(TuriEnv *env, const char *name,
                                    TuriNativeFn fn, void *ud,
                                    const char *signature /* e.g. "(:int) -> :float" */);
```

Store the parsed signature on `TuriClosure`, then in
`elab_call.c:2386`, replace the allow-list with a lookup against the
registered signature for the current env. Arg types and return type
both become first-class.

Open design questions:

- **Signature format.** A small parser (`(:int :cstr ...) -> :float`)
  vs. structured `TuriType arg_tys[N], TuriType ret_ty`. Structured is
  less rope; the parser would have to handle parametric / opaque types
  embedders have, which adds scope.
- **Polymorphism.** Initial scope: monomorphic. Embedder natives that
  want polymorphism stay on the untyped path with `dispatch_result =
  TYPE_INT`.
- **Env-vs-process scope.** `turi_register_default_native` registers
  process-globally; the typed variant must too. Per-env overrides take
  precedence as today.
- **Compat.** Old `register_native` keeps working (still typed as
  `:int`). Embedders opt in.

### B. Per-embedder allow-list extension (short-term unblock)

Grow the `elab_call.c:2386` allow-list with the godot-* natives whose
types are known and stable. Brittle, but cheap and lets the
turmeric-godot prelude grow today without touching the registration
API.

Concrete starter set:

```c
else if (strcmp(nm, "godot-vec2-x") == 0)     dispatch_result = TYPE_FLOAT;
else if (strcmp(nm, "godot-vec2-y") == 0)     dispatch_result = TYPE_FLOAT;
else if (strcmp(nm, "godot-vec3-x") == 0)     dispatch_result = TYPE_FLOAT;
else if (strcmp(nm, "godot-vec3-y") == 0)     dispatch_result = TYPE_FLOAT;
else if (strcmp(nm, "godot-vec3-z") == 0)     dispatch_result = TYPE_FLOAT;
else if (strcmp(nm, "godot-color-r") == 0)    dispatch_result = TYPE_FLOAT;
/* ... */
```

Rejected sub-option: hardcoding the godot-* prefix in `elab_call.c`
ties the compiler to one embedder. If we go short-term, gate on a build
flag or load the list from a config.

### C. Don't fix; document and live with the constraint

Embedder scripts call godot-* natives directly for non-`:int` returns,
forfeiting the curated facade for accessors. This is the current
behavior. The cost compounds as the facade grows -- by the time we want
the full ~30-type spice, B or A is the only honest path.

---

## Recommendation

**A** is the right end state and fits the existing
`turi_register_default_native` shape. The short-term cost is one new
public function + a TuriClosure field + a lookup in elaboration. Until
A lands, the turmeric-godot prelude (and any future spice-side facade)
stays restricted to wrappers whose declared types happen to match
`:int`.
