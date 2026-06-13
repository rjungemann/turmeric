---
title: polymorphic `(ok x)` from Prereq 3 fails to compile when `x` is a value-struct payload (no carrier-box helper)
category: Stdlib / ABI gap
severity: Medium. Blocks the macro-emitted Decode side of `derive-json` for `defstruct` types in `../turmeric-spices/spices/json`, and any future return-type-dispatched class instance that wants to wrap a user-defined struct value in `(ok ...)`. Hand-written instances using inline-C `tur_ok((int64_t)(intptr_t)heap_user)` still work, so the surface is functional today; this is the last gap before `derive-json` can emit the Decode side mechanically.
description: Prereq 3 made `stdlib/result.tur`'s `ok` polymorphic via `(defn ok [A B] [x : A] : (Result A B) ```c return tur_ok((int64_t)(intptr_t)x); ```)`. The inline-C `(int64_t)(intptr_t)x` cast is correct for any A whose C-level representation is a scalar or pointer (int, bool, cstr, opaque-int newtypes, parameterized struct via carrier ABI), but fails to compile for A that's a value-struct (non-parametric `defstruct` whose C-level representation is the struct by value). The user-side symptom is `error: passing 'User' (aka 'struct User') to parameter of incompatible type 'int64_t' (aka 'long long')` when calling `(ok user)` where `user` is a User struct value.
status: PARTIALLY RESOLVED 2026-06-13 as Prereq 6. The standalone call shape `(let [r : (Result T B) (ok (make-struct T ...))] ...)` is fixed: when a polymorphic constructor with an inline-C body is monomorphized AND any param is a value-struct AND the spec's C return type really is the by-value Result struct (not the int64 carrier) AND the function is `ok` or `err`, the emit pass synthesizes a direct by-value Result struct construction body. The remaining gap is the typeclass-instance-method context: when the spec is interned from a call site inside an instance method body, its result_type lowers to the int64 carrier (per the dispatch shim convention) and the synthesized direct-struct emission would mismatch the int64 signature. The original carrier-box helper sketch also fails there because tur_ok's int64 ok_val slot can't hold a multi-byte value-struct, and dereferencing the carrier as the by-value Result__A__B struct reads off the end. **What works**: standalone calls + main-repo regression fixture (`tests/fixtures/polymorphic-ok-err-value-struct-payload/`). **What's still gated**: `derive-json`'s Decode-side emission for defstruct types -- macro plumbing (`__decode-field` / `__decode-make-struct` in `../turmeric-spices/spices/json/src/json/encode.tur`) is preserved but the emission stays Encode-only until the instance-method carrier path is addressed.
---

# polymorphic `(ok x)` fails when x is a value-struct payload

## Summary

Prereq 3 (2026-06-12) made `ok` polymorphic by inlining a
`(int64_t)(intptr_t)x` cast on `x`. This works for any A whose
C-level value representation is int64-sized:

- scalars: `int`, `bool`, sign-/zero-extended numerics
- pointers: `cstr`, `ptr<void>`, opaque-int newtypes
- parameterized structs (carrier ABI): `(Result int cstr)`,
  `(Pair A B)`, `(Vec A)` -- these are already int64 heap-pointer
  carriers at the value level, so the cast is a relabel.

It fails for non-parametric `defstruct` values, whose C-level
representation is the struct by value (pass-by-value below ~16 bytes,
pass-by-pointer above per Phase D). The inline-C cast `(int64_t)(intptr_t)x`
on a struct rvalue is invalid C (struct-to-integer conversion).

## Where this surfaces

The canonical case is the Decode side of `derive-json` in the json
spice. The macro would naturally emit:

```turmeric
(definstance Decode [User]
  (decode [doc val]
    (ok (make-struct User
                     <decoded-id>
                     <decoded-name>))))
```

which lowers to roughly:

```c
return ok((User){
    .id   = ok_val(...),
    .name = ok_val(...),
});
```

Clang rejects: `error: passing 'User' (aka 'struct User') to parameter
of incompatible type 'int64_t' (aka 'long long')` -- because
`static int64_t ok(int64_t)` and a `User` struct rvalue do not
inter-convert.

The Encode side of `derive-json` has the matching shape but doesn't
need `ok` -- it returns a cstr fragment via `__json-obj-build`, so the
gap shows up only on the Decode side.

## Workaround now in tree

`../turmeric-spices/spices/json/src/json/encode.tur`:

- The Decode-side macro helpers (`__decode-field`,
  `__decode-fields`, `__decode-make-struct`) are present and exported.
- `derive-json` emits the Encode instance only; the Decode instance
  emission is gated behind a comment that points at this report.
- Hand-written `(definstance Decode [T] ...)` for user defstructs
  works today. The pattern is documented in the `derive-json`
  docstring; the body uses inline-C to allocate a heap copy of the
  struct, fill its fields from per-field Decode dispatches, and
  return `tur_ok((int64_t)(intptr_t)heap_struct)`.

So the typed Decode surface is *usable*: it just needs more hand-work
than the Encode side until this report is addressed.

## Root cause

`stdlib/result.tur:27`:

```turmeric
(defn ok [A B] [x : A]
  #{}
  : (Result A B)
  ```c
  return tur_ok((int64_t)(intptr_t)x);
  ```)
```

The inline-C body is shared across all monomorphizations of A. The
cast `(int64_t)(intptr_t)x` assumes `x` is an int64-castable value
(scalar or pointer). For a value-struct A, the monomorphizer emits the
function with `User x` as the parameter and the cast `(int64_t)(intptr_t)x`
on a struct rvalue is invalid C.

The same hazard exists for `err` (symmetric monomorphization on B) and
for the analogous `some` in `stdlib/option.tur` -- though `some` was
*not* made polymorphic in Prereq 3 (still `(defn some [x : int] : int ...)`
as of 2026-06-13), so it doesn't currently hit this case.

## Proposed fix: compiler-level per-monomorphization boxing (inferred from types)

The right shape is at the codegen layer, not the stdlib, and the
recognizer is purely type-driven -- no source-level annotation. The
monomorphizer already knows A's concrete representation when it emits
`ok__spec__<A>_<Result>`; it should adapt the body per A rather than
routing every shape through a shared inline-C cast that was written
assuming scalar/pointer A. Sketch:

1. **Detect at monomorphization.** In the codegen pass that emits
   `<name>__spec__<A>_<...>` shims for polymorphic stdlib defns, the
   trigger is the conjunction of three existing predicates -- no new
   metadata required:

   - `fd->body_is_inline_c` (the body is opaque C text shared across
     all monomorphizations of A, so the codegen can't trust it to
     handle a struct rvalue).
   - A's resolved type is a non-parametric `defstruct` whose value
     representation is NOT the int64 carrier:
     `!type_uses_carrier_abi(A) && A.kind == TY_STRUCT &&
      A.as.struct_.def && A.as.struct_.def->n_type_params == 0`.
   - The function's return type uses the carrier ABI (the spec returns
     int64). `type_uses_carrier_abi(spec->result_type)`.

   All three together identify the exact shape that breaks: a
   polymorphic constructor whose body assumes int64-shaped `x` is
   getting monomorphized for a struct-by-value A and is producing a
   carrier handle. Everything else falls through unchanged.

2. **Emit a wrapper body that boxes by value to a heap pointer**,
   rather than relying on the source-level inline-C cast. For
   `(defn ok [A B] [x : A] : (Result A B) ```c return tur_ok(...); ```)`
   the value-struct monomorphization emits:
   ```c
   static int64_t ok__spec__User_Result__User__cstr(User x) {
       User *__h = (User *)malloc(sizeof(User));
       *__h = x;
       return tur_ok((int64_t)(intptr_t)__h);
   }
   ```
   instead of the shared `return tur_ok((int64_t)(intptr_t)x);` body
   (which clang rejects on the User rvalue). The pass-by-ptr case
   (Phase D structs > 16 bytes) gets the analogous `const T *x`
   parameter with `User *__h = malloc(...); *__h = *x;`.

3. **Compose with the existing carrier bridge.** Downstream
   `ok-val r` consumers route through `(*(Result__User__cstr
   *)(intptr_t)<carrier>).ok_val` via the Prereq 2 carrier->concrete
   bridge -- which already works correctly on this shape because
   `Result A B` IS a parameterized struct (carrier ABI). The new
   monomorphization only affects how the *argument* gets into the
   carrier; the rest of the chain is unchanged.

4. **Generalize automatically.** The same pattern fires for `err`,
   `some` (once polymorphic), `cons`, and any future polymorphic
   stdlib constructor whose A can resolve to a value-struct -- the
   three predicates above catch them all without per-name special-
   casing. Pure-Turmeric bodies (`make-struct Pair a b` etc.) skip
   the wrapper because `body_is_inline_c` is false; their existing
   monomorphization already lowers value-struct A correctly.

### Why no `#{ConstructByBoxing}` marker

An earlier draft of this report proposed an effect-style marker
analogous to `#{Unsafe}` so the elaborator could record boxing intent.
That marker is unnecessary: the three type-level predicates already
identify the exact shape that needs boxing, and no realistic
polymorphic inline-C body wants the *other* behavior (a body
deliberately consuming a value-struct A would have to reference the
struct's field names, which can't be written portably in the
inline-C-shared-across-monomorphizations model -- the body would
fail to compile for any other A).

`#{Unsafe}` exists because effect rows are observable to call sites
and have to live in the signature; boxing is an internal codegen
detail that doesn't change the type or the call site's view. Keep it
in the codegen.

### Why not stdlib-level alternatives

Two narrower shapes were considered and rejected as the *target* of
the fix (they're fine as temporary workarounds if the codegen change
needs more design time):

- **Separate `ok-struct` / `err-struct` defns** that heap-spill
  inline-C. Localized but pushes the choice of constructor onto the
  call site (and onto `derive-json`'s macro expansion) -- so every
  macro that emits `(ok ...)` would need to know A's representation
  kind, which the macro layer doesn't easily introspect. Adds API
  surface where the user shouldn't see it.
- **Classify-type branch inside the existing polymorphic `ok`
  body** via `__builtin_classify_type` or `_Generic`. Keeps a single
  constructor but locks the stdlib body to a GCC/Clang-specific
  builtin (`__builtin_classify_type`) or to a `_Generic` form that
  doesn't dispatch on user-defined struct types. Either way the
  stdlib body grows a compiler-aware branch that the codegen should
  own.

The codegen-layer fix keeps the stdlib source body minimal -- the
source-level `(int64_t)(intptr_t)x` cast stays, and the monomorphizer
routes value-struct A through the boxing wrapper before the cast ever
runs. The pattern matches how Prereq 2 was solved (the bridge is at
the codegen layer, not in the source body of `ok-val`).

### Likely site

`src/compiler/emit_module.c` is where polymorphic-stdlib-defn
specialization emission lives (the same area that grew the
`tur_ok` / `tur_err` prelude generators around line 2155). The new
logic plugs in next to the existing per-spec emission: when the three
predicates in step 1 hold, swap the inline-C body for the box-and-carry
form before emitting the shim.

## Validation when fixed

Per-monomorphization box-and-cast at the codegen layer (the
direction above) should land along with these checks:

- **Main-repo regression fixture** under `tests/fixtures/` exercising
  `(ok (make-struct User 7 "alice"))` end-to-end: a value-struct
  payload is constructed, wrapped in `(ok ...)`, the resulting
  `(Result User cstr)` flows through `ok-val` (consuming the carrier
  via Prereq 2's bridge), and the User fields read back correctly.
  Symmetric fixture for `(err <user-struct>)` once we have a
  defstruct error type in tree.
- **No codegen-snapshot regressions** -- the monomorphization swap
  only fires on previously-broken programs (value-struct A through
  polymorphic `ok`/`err`); existing scalar/pointer-A specializations
  keep emitting the shared inline-C body.
- **Spice-side**: flip `derive-json` in
  `../turmeric-spices/spices/json/src/json/encode.tur` to emit both
  Encode and Decode instances using the existing
  `__decode-make-struct` macro plumbing (kept in tree precisely for
  this -- the macro helpers are already exported). Add a
  `derive-decode-struct` fixture under
  `../turmeric-spices/spices/json/tests/` that parses JSON, decodes
  to a User via `(:: (decode doc root) (Result User cstr))`, then
  encodes back -- collapsing to a single
  `(derive-json User (id int) (name cstr))` declaration with no
  hand-written instance.
- **Generalize check**: once `some` is made polymorphic (currently
  monomorphic on `:int` in `stdlib/option.tur`), the same
  monomorphization wrapper should apply automatically. Pin this with
  a `(some (make-struct User ...))` fixture and verify no per-stdlib-
  defn special-casing was needed.
