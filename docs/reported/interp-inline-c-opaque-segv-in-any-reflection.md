---
title: Under `--interpret`, `type-of` on an `any` holding an inline-C-produced opaque value dereferences the value as a struct pointer and segfaults
category: Reported
description: CRASH FIXED by direction 2. Direction 1, the "real fix", turned out to be blocked: FnDef.return_type carries only a bare TypeKind for a plain ADT return -- measured NULL def for an opaque AND for a genuine defdata -- so there is nothing at the tag site to distinguish them. The residue is a back-end divergence: interpreted answers `adt` where compiled answers `Route`.
---

# An inline-C opaque value crashes the interpreter's `any` reflection

**CRASH FIXED 2026-09-08** by direction 2 (harden the reader).
`tests/run-interp-inline-c-opaque.sh` / ctest `tur_interp_inline_c_opaque` pins
it, and was verified to FAIL against the unfixed reader.

**Direction 1 -- the one this report called "the real fix" -- is blocked, and
the reason is the useful part.** See below. What remains is a back-end
divergence, not a crash: interpreted answers `adt` where compiled answers
`Route`.

**Severity was high (crash), narrow (one path).** A hard segfault, not a wrong
answer -- but it needed only `type-of` on a value a stdlib-style `defopaque`
constructor produced.

Found while fixing
[interp-collection-handles-report-as-int](../archive/interp-collection-handles-report-as-int.md):
the first probe for that report used an inline-C `defopaque` constructor, which
is how it surfaced. It is **pre-existing and unrelated** -- the guard it trips
predates this session's `any` work, and the crash reproduces with no boxing
involved.

## Repro (2026-09-07)

```turmeric
(defopaque Route :int)
(defn route-of [n : int] : Route
  ```c
  return n;
  ```)
(defn boxed [] : any (route-of 7))
(defn main [] : int (println (type-of (boxed))) 0)
```

```
$ tur run p.tur
Route

$ tur --interpret p.tur
src/turi/eval.c:1169:20: runtime error: member access within misaligned address
  0x000000000007 for type 'struct TuriStruct', which requires 8 byte alignment
AddressSanitizer: SEGV on unknown address 0x000000000001f
```

Replacing the inline-C constructor with an ascription -- `(:: 7 Route)` -- does
not crash, which localises it to the inline-C return path rather than to
`defopaque` itself.

## Root cause

`turi_any_named_type` validates only the tag and non-NULLness before
dereferencing:

```c
static const char *turi_any_named_type(TuriValue v) {
    if (v.tag != TURI_STRUCT || !v.as_struct) return NULL;
    if (!turi_struct_is_struct_like(v) && v.as_struct->ctor && ...
```

A pointer of `7` passes both checks. The value comes from the interpreter's
inline-C path: the body returns the plain integer `n`, and because the declared
return type is an opaque (a lowered ADT), the result is tagged `TURI_STRUCT`
while the payload is still the immediate. Nothing reconciles the two.

## Fix directions -- 1 blocked, 2 taken

1. ~~**Do not tag an inline-C result as a struct when the body returns an
   immediate.**~~ **BLOCKED, measured.** The re-tag site
   (`eval.c`, the `try_exec_simple_inline_c` result path) fires on
   `fn->return_type.kind == TY_ADT`, and the obvious fix is to exempt an
   opaque. It cannot: **`FnDef.return_type` carries no `AdtDef`.** Probed at
   that exact site:

   | case | `return_type.kind` | `def` | value |
   |---|---|---|---|
   | `(defopaque Route :int)`, inline-C returns 7 | `TY_ADT` | **NULL** | 7 |
   | `(defdata Shape ...)` round-tripped through inline-C | `TY_ADT` | **NULL** | 91328191639424 |

   Both NULL, so `is_opaque` is unaskable and there is no type-level signal
   separating "opaque, the word IS the value" from "real ADT, the word is a
   pointer". `type_name` on either prints the bare `<adt>`.

   It is not a bug in that field so much as its documented job.
   `elab_fns.c` sets it as

   ```c
   fd->return_type = return_borrow_type ? *return_borrow_type
                    : (return_app_type && return_app_type->kind == TY_APP ...)
                      ? *return_app_type
                      : type_from_kind(return_kind);
   ```

   with the comment "the bare kind is sufficient for the lifetime pass" -- the
   field exists for borrow lifetimes (LS2), and a plain ADT return keeps only
   its kind. No full `Type` for the return is in scope there; `return_kind` is a
   bare `TypeKind` throughout that function. Threading the declared type through
   is a real change to a large function and wants its own report.

2. **Harden the reader. DONE.** `turi_any_named_type` and
   `turi_struct_is_struct_like` both dereferenced an unvalidated pointer; they
   now share `turi_struct_ptr_is_plausible`. The two checks are facts, not
   heuristics: a `TuriStruct` requires 8-byte alignment, so an unaligned word is
   definitively not one, and the zero page is never mapped, so a word below it
   is definitively not one. Neither can reject a real `TuriStruct *`.

## The residue: a divergence, and one case still unguarded

Interpreted, `type-of` on the repro now answers **`adt`**; compiled it answers
**`Route`**. The struct name is only an OVERRIDE, and declining it falls back to
the generic ADT answer rather than to the opaque's name -- naming it needs the
`AdtDef` direction 1 cannot reach.

(Worth noting because it was guessed wrong first: the fallback does NOT recover
"Route" from the widen's static type. The ascription spelling `(:: 7 Route)`
answers "Route" for a different reason -- it produces a plain `TURI_INT`, so the
struct override never applies at all.)

And the hardening is partial by construction: an opaque over a LARGE integer is
bit-indistinguishable from a heap pointer, so it still mis-tags and would still
dereference garbage. Only attaching the def fixes that. The repro's `7` is the
realistic shape -- a small handle, an index, an fd -- but the guarantee is
"no crash for implausible pointers", not "no crash".

## Why nothing caught it, and what now does

`tests/run-turi.sh` PASS-skips every fixture containing a user inline-C block
(the TI7 carve-out, ~767 fixtures), so no FIXTURE in the tree can exercise an
inline-C `defopaque` under `--interpret` at all. That is why this needed a
dedicated runner rather than a fixture, and it is worth remembering as a blind
spot: the carve-out hides an entire interaction, not just some cases of it.

`tests/run-interp-inline-c-opaque.sh` asserts three things -- no crash, the
compiled path still answers `Route`, and a genuine `defdata` round-trip still
reaches its `match` arm (the case the struct re-tag exists for, and the one a
careless fix would break). It asserts the `adt` divergence explicitly rather
than hiding it, so closing direction 1 later will fail this runner and force the
expectation to be updated deliberately.
