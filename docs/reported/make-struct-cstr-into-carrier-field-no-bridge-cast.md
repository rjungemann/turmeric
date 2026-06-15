---
title: make-struct emits a -Wint-conversion warning for a cstr value in an int64 carrier field
severity: Low. Not a runtime miscompile -- the int64 field is wide enough to hold the pointer bits and the value round-trips correctly -- but it is type-unsafe C (an implicit pointer->integer conversion the C compiler warns on), and it defeats the "every conversion is a real cast clang can check" goal of the monomorphization plan. Caught while exercising the :heap make-struct path (Vec typed-pointer vertical slice, step 2).
status: OPEN (noted, not yet fixed). Sidestepped in the step-2 fixture by using int/bool element types; does NOT affect the real Vec migration (the Vec header fields are data/len/cap, never element-typed cstr).
---

# make-struct: cstr value into an int64 carrier field lacks the intptr bridge cast

## One-line

`(make-struct S :field "literal")` where `S`'s field lowers to the int64 carrier
(a `:A` tyvar field, or any field whose C type is `int64_t`) emits
`.field = "literal"` -- an implicit `const char * -> int64_t` conversion the C
compiler flags with `-Wint-conversion`. The pointer-bridge in the make-struct
emitter covers `TY_PTR_VOID`/`TY_RC`/`TY_REF`/`TY_WEAK`/`TY_EXISTS`/`TY_FORALL`
but **not `TY_CSTR`**.

## Repro

```turmeric
(defstruct Box :heap [A] (val :A) (tag :int))
(defn main [] : int
  (let [bc (:: (make-struct Box :val "hello" :tag 9) (Box cstr))]
    (println (:: (.val bc) cstr)))
  0)
```

`tur build` of this prints (and still produces a working binary):

```
warning: incompatible pointer to integer conversion initializing 'int64_t'
         with an expression of type 'char[6]'
    *__t = (Box__cstr){.val = "hello", .tag = INT64_C(9)};
                              ^~~~~~~
```

(With A=int or A=bool there is no warning -- the value already lowers to int64.)

## Root cause

`src/compiler/emit_expr.c`, `EX_MAKE_STRUCT` field-assignment loop. The
"bridge a pointer value into an int64 carrier field" branch tests:

```c
bool val_is_c_ptr = (vek == TY_PTR_VOID || vek == TY_RC
                     || vek == TY_REF || vek == TY_WEAK
                     || vek == TY_EXISTS || vek == TY_FORALL);
```

`TY_CSTR` is absent, so a `const char *` value assigned to an int64 carrier
field skips the `(int64_t)(intptr_t)` cast and is emitted bare.

The field's C type is the int64 carrier whenever the field is a `:A` tyvar (the
element is carried as int64 inside the struct -- the documented Vec/collection
element-carrier convention) or otherwise `int64_t`.

## Why it surfaced now

Before the `:heap` make-struct path (Vec typed-pointer vertical slice, step 2),
stdlib collection constructors stored elements via inline-C helpers
(`vec-push!`) whose `int64_t val` parameter received the cstr through the
call-site arg coercion (which DOES cast). `make-struct` with a cstr value
flowing into a tyvar/carrier field is newly reachable now that `make-struct`
itself can be the constructor for a parameterized heap type.

## Fix direction

Add `TY_CSTR` to `val_is_c_ptr` so the value is bridged as
`(int64_t)(intptr_t)<cstr>`. This is safe and narrow: the branch only fires when
`!field_is_c_ptr` (the field is NOT a cstr/pointer slot), i.e. exactly the
int64-carrier case. A cstr value into a genuine cstr field (`field_is_c_ptr`
true, which already lists `TY_CSTR`) is unaffected.

## Validation

- The repro above builds warning-free.
- Snapshot impact: only fixtures whose `make-struct` assigns a cstr value into a
  non-cstr (carrier/tyvar) field change; regen those `expected.c` in the same
  PR. (Likely a small set, possibly zero outside the new heap path.)
- Does not affect the Vec migration: the Vec header has no cstr field.

## Related

- [docs/upcoming/v2/vec-typed-pointer-vertical-slice-plan.md](../upcoming/v2/vec-typed-pointer-vertical-slice-plan.md)
- CLAUDE.md "No Lazy `:int` Stand-Ins" / type-safety-at-the-C-boundary goal of
  the end-to-end monomorphization plan.
