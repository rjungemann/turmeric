# `clone_struct_app_type` segfaults on a TY_APP with a null argument

**Severity: high** -- a compiler **crash**, with no diagnostic and no output.
Any program using a parametric `definstance` head whose method recurses into
the type parameter took down `tur` in `memmove`.

**Status: RESOLVED** 2026-09-12. Found executing
[crdt-spice-plan.md](../upcoming/crdt-spice-plan.md) C3, whose `ORMap` design
depends on exactly this shape.

## Repro

```turmeric
(defclass JS [a] (j [x : a y : a] : a))
(defstruct Box [A] [v : A])
(definstance JS [(Box A)] (j [x y] (Box (j (.v x) (.v y)))))
(defn main [] : int 0)
```

```
$ tur check p.tur
AddressSanitizer: SEGV on unknown address 0x000000000000
    #1 clone_struct_app_type types.c:1191
    #2 substitute_adt_app_type_owned types.c:1403
    #3 adt_field_c_type types.c:1647
    #4 record_adt_app_ctor_sigs types.c:1868
    #5 type_register_adt_app types.c:1903
```

It needs all three of: a parametric `defstruct`, a parametric instance head,
and a body that calls the same class method on a field. Drop the recursive
call and it compiles; make the struct monomorphic and it compiles.

## Root cause

```c
Type clone_struct_app_type(Type t) {
    if (t.kind != TY_APP) return t;
    ...
    *out.as.app.fn  = clone_struct_app_type(*t.as.app.fn);   /* fn may be NULL */
    *out.as.app.arg = clone_struct_app_type(*t.as.app.arg);  /* arg may be NULL */
```

Both are dereferenced unguarded. A `TY_APP` reaching here during instance
registration can carry a null `fn`/`arg` -- a partially-resolved application
whose argument is not bound yet.

The decisive evidence that this state is *expected* rather than corruption:
`free_struct_app_type`, the paired function **directly below it in the same
file**, has always null-checked both fields. The clone was simply asymmetric
with its own free.

## Fix

Mirror the free's null checks in the clone (`src/compiler/types.c`). Both
fields are initialized to `NULL` and cloned only when present.

## What it unblocked

More than the crash. With the guard in place the shape *works*:

```turmeric
(definstance JS [int] (j [x y] (if (< x y) y x)))
(definstance JS [(Box A)] (j [x y] (Box (j (.v x) (.v y)))))

(let [r (:: (j (Box 3) (Box 7)) (Box int))] (println (.v r)))   ; => 7
```

The outer instance dispatches, recurses at the element type, and reaches the
`int` instance. That is the general "container CRDT merges by merging its
elements" form -- an `ORMap` of counters joining by joining each value.

## Remaining limits (NOT regressions; untouched by this fix)

- **Nesting two deep** -- `(Box (Box int))` fails in codegen with
  `passing 'tur_adt_Box__int' to parameter of incompatible type 'int64_t'`:
  a by-value struct where the carrier is expected.
- **The result needs an ascription** -- `(.v (j (Box 3) (Box 7)))` reports
  `no typeclass method found for 'v'`; the return-directed method's type has to
  be pinned with `(:: ... (Box int))` first. Pre-existing and general.
- **A caret-spelled instance constraint is not recognized** --
  `(definstance JS [(Box A)] [^JS A] ...)` gives
  `constraint typeclass '^JS' is not defined`. Neither
  `elab_typeclasses.c:2907` nor `:3045` strips a leading `^` the way the
  *impl-parameter* path does. Not needed for the working shape above (the
  recursion resolves without a declared constraint), so it is recorded here
  rather than fixed.

## Fixture

`tests/fixtures/typeclass-parametric-instance-recursive` -- two element values
so a fix that works by accident at one carrier is not mistaken for a real one,
with the two-deep limitation noted in-file.
