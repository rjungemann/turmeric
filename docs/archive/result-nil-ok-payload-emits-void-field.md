---
title: "`(Result nil E)` / `(Option nil)` emit a `void` union field and do not compile"
category: Reported
description: "`nil` c-names to `void`, and three emission sites used that spelling verbatim for a union member, a ctor parameter, and a match binder. The signature type-checked and then failed at cc, naming only generated identifiers."
---

# `(Result nil E)` / `(Option nil)` emit a `void` union field and do not compile

**RESOLVED 2026-09-16.** All three sites, and a fourth the filing did not name.
The answer is the report's first fix direction taken one step differently: not
"treat the field as a zero-field constructor" (which would change constructor
ARITY, and so every call site and match arm) but "give the payload the int64
slot the erased twin already reads there". A zero-information payload needs no
storage; it needs a WELL-FORMED member, and every other arm of the union is
int64-shaped anyway. The bytes are dead by construction -- `nil` has no values
to read -- exactly like the other dead union arms the by-value ctor prologue
already leaves indeterminate.

- **The union member and the ctor parameter** both come from
  `adt_field_c_type` (`src/compiler/types.c`), which returned
  `type_c_name(resolved)` verbatim. It now maps `"void"` to `"int64_t"`, next
  to -- and for the same reason as -- the sub-word widening directly below it:
  the monomorph's member and the generic layout every erased reader goes
  through have to agree on the slot.
- **The match binder** is `match_binder_c_type` (`src/compiler/emit_expr.c`),
  shared by the direct emitter's three binder sites and `emit_cps_ir.c`'s
  mirror. The mirror is the one that actually emitted the failing line --
  stdlib's `ok?` compiles through the CPS path -- so patching only the direct
  emitter left the defect exactly where it was.
- **The fourth site** is the SR2a/SR2b override inside the switch-path binder,
  which re-derives the binder's C type from the monomorph's substituted field
  and put `void` back after the binder had already been normalised. Routed
  through the same helper, which also makes its `!= "int64_t"` guard decline to
  override at all -- the right outcome, since the binder already names the slot.

Pinned by `tests/fixtures/result-nil-ok-payload`: both `(Result nil int)` and
`(Option nil)`, an inline-C producer and a Turmeric-side one, the accessors, an
explicit `match`, and the error payload checked in every case -- a wrong slot
would print `0` rather than fail to compile.

**Spice-side follow-up: done** (turmeric-spices#75).
`turmeric-spices/spices/nng` shipped `(defopaque Ack :int)` as the stand-in this
report exists to retire; `dial` / `listen` / `sub-subscribe` and both timeout
setters now return `(Result nil int)`, and the opaque, its export and its doc
block are gone. The emitted monomorph is

```c
typedef struct tur_adt_Result__nil__int {
    int tag;
    union {
        struct { int64_t _0; } Ok;
        struct { int64_t _0; } Err;
    } as;
}
```

-- the Ok slot dead by construction, which is the point.

**Severity when filed: low-medium.** The failure is loud -- three C compiler errors about
generated identifiers -- and there is a cheap workaround. But `nil` is the
correct type for "this operation either worked or failed, and success carries
nothing", which is the return shape of every setter, connector, and binder in a
C-wrapping spice. With it unavailable, each such API reaches for `(Result int
int)` and a "ok carries 0" convention -- exactly the `:int` stand-in that
`CLAUDE.md` exists to prevent -- or invents a per-spice `Ack` opaque.

**Status when filed: open.** Found writing the `nng` spice, whose plan
(`docs/upcoming/nng-spice-plan.md`) specified `(Result nil int)` for `dial`,
`listen`, and the timeout setters. The spice ships a `(defopaque Ack :int)` as
the stand-in, documented as such, and should switch to `nil` when this lands.

## Repro

```turmeric
(defmodule repro/nilres (export)

(defn f [x : int] : (Result nil int)
  ```c
  if (x != 0) return tur_err_int((int64_t)x);
  return tur_ok_int(0);
  ```)

(defn main [] : int
  (if (ok? (f 0)) (println "ok") (println "no"))
  0)
)
```

```
$ tur build nilres.tur -o nilres
nilres_tur.c:3786:23: error: field has incomplete type 'void'
 3786 |         struct { void _0; } Ok;
nilres_tur.c:3794:63: error: argument may not have 'void' type
 3794 | static tur_adt_Result__nil__int ctor_Result_Ok__nil__int(void _0) {
nilres_tur.c:8090:22: error: variable has incomplete type 'void'
 8090 |                 void _un_1110 = (void)__scrut->as.Ok._0;
```

`(Option nil)` fails identically -- `struct { void _0; } Some;` and
`ctor_Option_Some__nil(void _0)`. The producer does not have to be inline C;
what matters is that a monomorph over `nil` gets emitted at all.

`tur check` passes on both. Nothing in the type system objects -- `nil` is a
real `TypeKind` (`TY_NIL`, `src/compiler/types.c:499`), and the elaborator
accepts it as a type argument like any other. The rejection is entirely in the
emitted C.

## Root cause

`TY_NIL`'s C spelling is `void` (the first column of its row in the `types.c`
table above). Three emission sites use a payload's C type without asking
whether it is inhabited:

1. **The union field** -- `emit_module.c:9388-9391` writes
   `struct { %s _0; ... }` per constructor from `adt_ctor_field_c_type`, so a
   `nil` payload lands as `void _0`.
2. **The constructor function** -- the same monomorph's `ctor_<Adt>_<Ctor>`
   takes one parameter per field, so it gets a `void` parameter.
3. **The pattern-match binder** -- the destructuring arm emits
   `void _un_N = (void)__scrut->as.Ok._0;` for the payload it is not going to
   use.

All three are the same question: a zero-information payload occupies no storage,
takes no argument, and binds no variable.

## Fix directions

Treat a `nil`-typed constructor field as a **zero-field** constructor at
emission time, at all three sites: skip it in the union struct (a constructor
whose only field is `nil` becomes `struct { } Ok;`, or better, an empty
placeholder consistent with however the emitter already spells a nullary
constructor), drop the parameter from the `ctor_` signature, and emit no binder
in the match arm.

The nullary path already exists -- `(Result int int)`'s `Err` arm and every
enum-shaped ADT go through it -- so the change is routing `nil` fields into it
rather than inventing a representation.

Worth deciding at the same time whether `nil` should be admissible as a type
ARGUMENT at all, or whether the language wants a real `Unit` with a value. There
is none today (`grep -rn 'defstruct Unit\|TY_UNIT'` finds nothing), and
`(Result nil E)` appears nowhere in `stdlib/` -- so this shape has simply never
been exercised, which is why it type-checks and then fails at `cc`.

If the answer is "not admissible", the fix is a diagnostic at the type
application, not at `cc` -- the current failure names `tur_adt_Result__nil__int`
and a line in a generated file, which tells a spice author nothing about the
signature they wrote.
