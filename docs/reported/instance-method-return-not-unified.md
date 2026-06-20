---
title: Instance-method body result type is not unified with the declared method return type
category: Type checking -- typeclass instance elaboration (elab_definstance pass 2)
severity: Medium. A `definstance` method may return a value of a completely
  unrelated type (an int, a cstr, an unrelated struct) where the typeclass
  signature declares something else, and `tur check` / `tur build` accept it
  with no diagnostic. The body IS elaborated (an unknown call inside is still
  caught), so this is a missing *unification*, not a missing *elaboration*.
  Surfaces concretely in the http-handler typeclass work (a handler instance
  whose `handle` is declared to return `Response` can return a non-Response
  and type-check). Consistent with the Track-A carrier-ABI bridge, where the
  dispatch shim funnels every method result through an int64 carrier.
status: OPEN
---

# Instance-method return type is not checked against the method signature

## One-line summary

When elaborating a `definstance` method body, the compiler never unifies the
body's synthesized result type against the method's **declared return type**
(taken from the enclosing `defclass` signature). An instance method declared
`: cstr` can `42` (int), a `: Response` method can return an unrelated
struct, etc., and the program type-checks clean.

## Repro (verified on this tree, tur 0.21.0)

```turmeric
;; encode is declared : cstr, but the HandleX instance returns an int.
(defclass Encoder [W] (encode [^borrow w] : cstr))
(defopaque HandleX :int)
(definstance Encoder [HandleX]
  (encode [w] 42))            ;; <-- returns int, declared : cstr
(defn main [] : int 0)
```

```
$ ./build/tur check encode-mismatch.tur
$ echo $?
0                              ;; expected: a type error (int vs cstr)
```

The body IS elaborated -- an unknown call inside the same instance method is
still rejected, which proves the body is walked, only its *type* is never
checked against the signature:

```turmeric
(definstance Encoder [HandleX]
  (encode [w] (totally-unknown-fn w)))
;; => error: unknown function or operator 'totally-unknown-fn'   (exit 1)
```

So the gap is precisely the final body-result-vs-declared-return unification,
not elaboration of the body.

## Root cause (file:line)

All in `src/compiler/elab_typeclasses.c`, function `elab_definstance`.

**Pass 1** computes the declared return type but only keeps its *kind*, and
then resets the FnDef's `return_type` to `TY_UNKNOWN`:

- `src/compiler/elab_typeclasses.c:3415`
  ```c
  Type fn_type = type_fn(param_kinds, n_method_params, return_type.kind);
  ```
  Only `return_type.kind` (a `TypeKind`) flows into the method's function
  type -- the full declared `Type` is dropped here. (The full type is only
  preserved into `fn_type.as.fn.result_full_type` in a few special cases:
  TY_FN arrow heads, nominal structs, TY_APP, TY_TYVAR.)

- `src/compiler/elab_typeclasses.c:3510`
  ```c
  method_fd->return_type = type_simple(TY_UNKNOWN, CK_COPY);
  ```
  The FnDef that will carry the body is given `TY_UNKNOWN` as its return
  type, so there is nothing left to unify the body against later.

**Pass 2** elaborates the body and stores it, but performs no unification:

- `src/compiler/elab_typeclasses.c:3610-3635`
  ```c
  Expr *method_body = e_nil(e, impl_form->span);
  ...
  method_body = elab_form(e, impl_form->as.list.items[impl_body_start]);   // n_body == 1
  ...
  FnDef *method_fd = mp->method_fd;
  method_fd->body = method_body;     // <-- body's actual type ignored
  ```
  No `e->expected_type` is set before `elab_form`, and no
  `type_unify(method_body->type, <declared return>)` (nor a `TUR-E*`
  diagnostic) runs afterward. The only post-elaboration return handling is
  the arrow-head refinement at `:3657-3670`, which *refines* a TY_FN result
  from the body but never *rejects* a mismatch.

Contrast with the ordinary `defn` path (`src/compiler/elab_fns.c`), which
sets `e->expected_type` to the declared return before elaborating the body so
the body is checked against it. Instance methods skip that step entirely.

## Why it has been invisible

This is consistent with the Track-A carrier-ABI bridge: the typeclass
dispatch shim returns an `int64` carrier and reinterprets it at the call
boundary (see `docs/archive/instance-method-return-carrier-bridge.md`). With
every method result funneled through a single int64 width, a wrong-typed
body still "fits" the carrier and nothing downstream complains. The
by-value migration narrows that escape hatch on the *emission* side, but the
*elaboration-time* return check is still absent.

## Fix directions

1. In pass 1, retain the (tyvar-substituted) declared return `Type` on the
   method FnDef -- e.g. store it on `method_fd->return_type` instead of
   overwriting with `TY_UNKNOWN` at `:3510`, or stash it alongside `fn_type`
   for pass 2 to read.
2. In pass 2, after elaborating `method_body` (`:3614` / `:3618-3624`),
   either set `e->expected_type` to that declared return before `elab_form`
   (mirroring `elab_fns.c`), or `type_unify(method_body->type, declared)`
   afterward and emit a `TUR-E*` mismatch on failure -- skipping the existing
   arrow-head refinement case at `:3657-3670`, which already handles TY_FN
   results.
3. Add a fixture under `tests/fixtures/` asserting the mismatch is rejected
   (e.g. `instance-method-return-mismatch-rejected/`), paired with a positive
   control where the body type matches.

## Notes / scope

- Verified against this checkout's `./build/tur` (0.21.0). The repros above
  are turmeric-side and self-contained -- no spice needed.
- Distinct from `docs/archive/instance-method-return-carrier-bridge.md`,
  which fixed a *codegen* deref for by-value struct returns; that report
  explicitly notes "the elaborator side already worked" for ascribed
  *dispatch* monomorphization. The gap here is the absence of a
  body-vs-signature *return-type unification* at elaboration time -- a
  different layer.
