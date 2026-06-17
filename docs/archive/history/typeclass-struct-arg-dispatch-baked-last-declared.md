---
title: Struct-argument typeclass dispatch resolves every call site to the last-declared instance
category: Codegen / Elaborator — instance resolution
severity: High. Silent miscompile for typeclass instances over distinct structs that share a C layout (any pair of single-field-int structs, single-cstr structs, etc.). Hard cc error for instances over structs with differing layouts. Surfaced while implementing Track C -- U2 (`tur-ansi` Color typeclass: `Color4` / `Color8` / `Color24` + `Fg`/`Bg`); the agent that built the fg/bg + Color4/8/24 collapse type-checked the code but bailed at codegen because every `render`/`sgr` call site emitted `__inst_<Class>_<method>_Color24` (the last-declared instance). The plan's safety claim was inverted into a miscompile, so the spice-side work was reverted -- correctly -- and the unblocker filed against turmeric.
status: RESOLVED 2026-06-17 (same session as report). Fix landed on `claude/relaxed-thompson-beyzri`.
---

# Struct-argument typeclass dispatch baked to last-declared instance

## Summary

A typeclass with a method that takes a (plain, non-`TY_APP`) struct
argument, instantiated over multiple distinct structs:

```turmeric
(defstruct Color4  [code : int])
(defstruct Color8  [idx  : int])
(defstruct Color24 [r : int  g : int  b : int])

(defclass Render [a] (render [x] : int))
(definstance Render [Color4]  (render [x] 4))
(definstance Render [Color8]  (render [x] 8))
(definstance Render [Color24] (render [x] 24))

(render c4)   ; expect 4   --> baked __inst_Render_render_Color24
(render c8)   ; expect 8   --> baked __inst_Render_render_Color24
(render c24)  ; expect 24  --> baked __inst_Render_render_Color24
```

Every call site resolves to `__inst_Render_render_Color24` (the
last-declared instance). With differing layouts that is a hard cc
error; with matching layouts (e.g. `Fg` / `Bg` both `[code : int]`)
it is a silent wrong-vtable dispatch.

## Root cause

`src/compiler/elab_typeclasses.c` `resolve_typeclass_method` walks the
instance env (head = last-registered, since
`typeclass_env_register_instance` prepends). For a non-primitive
(`KIND_ARROW`) receiver, the type-match check is `type_ok =
!inst_is_primitive` -- so every aggregate instance of the class
passes. Discrimination by struct/ADT identity existed only for the
`TY_APP`-vs-`TY_APP` case (head-constructor and arg-type checks, lines
~4121 and ~4147). A bare `TY_STRUCT` receiver against a bare
`TY_STRUCT` instance head had no identity check, so the first instance
in the (prepended) list always "matched" first.

This is the same family as the previously-fixed
`turi-generic-dict-dispatch-bakes-representative-instance` (interpreter
side) and `m5-constrained-poly-wrong-instance-on-tyvar-receiver`
(tyvar receiver), but for *concrete* struct receivers -- the path that
should have been the easiest case to get right.

## Fix

`src/compiler/elab_typeclasses.c`: extend the `KIND_ARROW` type-match
branch with a struct-identity check (and the symmetric ADT check) for
plain (non-`TY_APP`) receivers, mirroring the existing `TY_APP`-vs-
`TY_APP` discriminators. NULL `def` (carrier / erased / abstract-tyvar
heads) still acts as a wildcard so the constrained-generic path is
unchanged.

```c
if (type_ok && obj->type.kind == TY_STRUCT && itk == TY_STRUCT &&
    obj->type.as.struct_.def && inst->type_args[0].as.struct_.def &&
    obj->type.as.struct_.def != inst->type_args[0].as.struct_.def) {
    type_ok = false;
}
if (type_ok && obj->type.kind == TY_ADT && itk == TY_ADT &&
    obj->type.as.adt_.def && inst->type_args[0].as.adt_.def &&
    obj->type.as.adt_.def != inst->type_args[0].as.adt_.def) {
    type_ok = false;
}
```

## Validation

- New regression fixture `tests/fixtures/typeclass-struct-arg-dispatch/`
  exercises both the differing-layout case (`Color4`/`Color8`/`Color24`)
  and the identical-layout silent-miscompile case (`Fg`/`Bg`); both now
  emit `__inst_<Class>_<method>_<TheseStruct>` per call site and produce
  `4 / 8 / 24 / 31 / 41` instead of the pre-fix "passing `Color4` to
  parameter of incompatible type `const Color24 *`" cc error or the
  silent-`Bg`-wins miscompile.
- Compiled-fixture suite: 1671 passed, 0 failed (was 1670 before the
  new regression fixture).
- Confirmed unblocking: the Track C -- U2 spice-side `tur-ansi` Color
  typeclass collapse (fg/bg + Color4/8/24) can now compile correctly
  without inverting its own safety claim.
