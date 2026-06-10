---
title: Generic parametric-struct-by-value lowering is internally inconsistent -- make-struct boxes into a carrier pointer while (.field t) reads it by value
category: Reported
severity: medium
status: OPEN -- pre-existing; masked in whole-program mode by specialization/pruning, exposed under separate compilation (`tur build <dir>`) which emits every generic defn verbatim.
description: For a parametric struct (n_type_params > 0) erased to the int64_t carrier, EX_MAKE_STRUCT emits an invalid `(int64_t){.e1=..}` compound literal, while EX_GET_FIELD's `through_carrier` path reads it as a heap pointer `((Name *)(intptr_t)v)->field`. The two sides disagree on the carrier representation, so the generic (unspecialized) form of a function like tuple.tur's `tuple2`/`tuple2-1st` cannot be emitted as valid, self-consistent C.
---

# Generic parametric-struct-by-value lowering is internally inconsistent

## Summary

**Severity: Medium.** The C lowering of a *parametric* struct (one with
`n_type_params > 0`, e.g. `(defstruct Tuple2 [A B] (e1 A) (e2 B))`) erases the
struct value to the `int64_t` carrier. The read and write sides of that carrier
disagree:

- **Write (`EX_MAKE_STRUCT`, `src/compiler/emit_expr.c`):** emits a C99 compound
  literal `(int64_t){.e1 = a, .e2 = b}` -- the struct's C type name resolves to
  the carrier `int64_t`, which is **not an aggregate**, so this is invalid C
  (`field name not in record or union initializer`).
- **Read (`EX_GET_FIELD`, `through_carrier` branch):** treats the same carrier
  as a **heap pointer**: `((Tuple2 *)(intptr_t)v)->e1`.

So the carrier is "a heap `Tuple2 *` cast to int64_t" on the read side but "a
by-value compound literal" on the write side. There is no single representation
that satisfies both, and the generic body cannot be emitted as valid C.

## Why it is normally invisible

Whole-program mode rarely emits the generic form: a call like
`(tuple2-1st (tuple2 3 4))` is constant-folded / inlined, and the unspecialized
`tuple2`/`tuple2-1st` defns are pruned (unreferenced after specialization).
`emit-c` of such a program contains no `tuple2` at all. Separate compilation
(`tur build <dir>`) compiles a library module by emitting **every** top-level
defn verbatim (nothing is pruned), so the generic `tuple2` body is emitted and
the inconsistency surfaces at the C-compile stage.

## Minimal repro

```sh
mkdir -p p/src && cd p
cat > build.tur <<'EOF'
(defpackage p :name "p" :version "0.1.0")
EOF
# A module that loads tuple.tur (whose generic tuple2 returns a parametric
# struct by value) and is compiled as a separate-compilation library.
cat > src/main.tur <<'EOF'
(load "stdlib/tuple.tur")
(defmodule main (defn main [] :int 0))
EOF
tur build .
```

Observed (per-module C-compile errors):

```
error: field name not in record or union initializer        // (int64_t){.e1=..}
error: request for member 'e1' in something not a structure or union
```

Expected: the generic `tuple2` / `tuple2-1st` emit valid, self-consistent C and
the module links.

## Root cause (file:line)

- `src/compiler/emit_expr.c`, `EX_MAKE_STRUCT`: builds
  `(%s){...}` with `struct_c_name = emit_type_c_name(ctx, e->type)`. For a
  parametric struct `e->type` lowers to `int64_t`, yielding the invalid
  `(int64_t){...}`.
- `src/compiler/emit_expr.c`, `EX_GET_FIELD`: the `through_carrier` branch
  (`type.kind == TY_STRUCT && def->n_type_params > 0`) emits
  `((Name *)(intptr_t)(sv))->field`, i.e. a heap-pointer read.

The two were written against different mental models of the carrier.

## Proposed fix directions

Pick a single carrier representation for parametric structs and make both sides
agree. The read side already assumes **heap pointer**, so the natural fix is to
make `make-struct` box:

```c
// when emit_type_c_name(e->type) is the carrier "int64_t":
Name *__t = (Name *)malloc(sizeof(Name));
__t->e1 = a; __t->e2 = b;
/* value is */ (int64_t)(intptr_t)__t
```

Care is needed because the generic function *parameter*/*return* ABI, the
`make-struct` site, and every `(.field t)` site must all agree, and the
specialized (monomorphized) path must not regress -- a naive gate on
`def->n_type_params > 0` changes behavior for specialized uses too. A first
attempt that boxed `make-struct` on the carrier (`emit_type_c_name == "int64_t"`)
fixed the constructor but then disagreed with a generic `(.e1 t)` that the same
function lowered as a *value* `(t).e1`, confirming the read side is itself
inconsistent across contexts (carrier vs value). A complete fix must unify the
parametric-struct carrier discipline across make-struct, get-field, and the
fn parameter/return ABI -- this is the "pre-existing
typeclass-dispatch-on-parametric-struct issue" the `stdlib/tuple.tur` and
`stdlib/pair.tur` comments already reference.

## How to validate

- The repro above compiles and links under `tur build <dir>`.
- Whole-program `bash tests/run.sh` stays green (the specialized path is
  unchanged).
- Add a separate-compilation fixture that emits a generic parametric-struct
  constructor + accessor and round-trips a value through them.

## Discovered while

Resolving the follow-ons of
[load-not-expanded-in-imported-or-project-modules.md](load-not-expanded-in-imported-or-project-modules.md):
arrow.tur's `__arrow_pair_*` helpers were made self-contained (a local
layout-compatible pair struct) precisely to avoid dragging tuple.tur -- and this
bug -- into project-mode arrow builds.
