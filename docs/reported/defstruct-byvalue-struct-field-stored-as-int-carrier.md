---
title: A defstruct field typed by a bare by-value struct/ADT is stored as the int64 carrier, so it neither reads back as its type nor accepts a by-value value
severity: medium -- the `:int` stand-in disease at the struct-field level. Because a `field : SomeStruct` slot is forced to the int64 carrier, both construction (`make-struct` with a by-value struct value) and access (`(.field x)` typed `:int`) are broken, so authors declare such fields `:int` (see tests/fixtures/clone-list `next : int`, typeclass-poly-wrapper-struct-receiver `Pos : int`).
status: open
discovered: 2026-06-17
surfaced-by: turmeric-spices ECS work (E2d), while fixing the nullary-opaque variant (defstruct-bare-user-type-field-reads-back-as-int-carrier.md). The opaque case was carrier-consistent and fixed; the by-value struct/ADT case is a deeper storage straddle and was scoped out.
---

# By-value struct/ADT defstruct field forced to the int64 carrier

## One-line summary

`elab_defstruct` lowers a field typed by a bare (nullary) user struct or ADT
to the int64 carrier (`fkind = TY_INT`) for STORAGE.  For an opaque newtype
that is the correct representation (now also carrying its nominal type for
read-back).  For a **by-value** struct/ADT it is a representation mismatch:
the C struct slot is `int64_t`, but the value flowing in is a by-value
aggregate.

## Minimal repro

```turmeric
(defstruct Inner [v : int])
(defstruct Outer [inner : Inner])

(defn use-inner [i : Inner] : int (.v i))

(defn main [] : int
  (let [o (make-struct Outer (make-struct Inner 7))]
    (use-inner (.inner o))))
```

Observed (pre-existing): `function 'use-inner' arg 1: expected Inner, got int`
-- `(.inner o)` reads back as the int64 carrier.  Recording the nominal
`full_type` to fix the read-back makes the *construction* fail instead:

```
error: incompatible types when initializing type 'long int' using type 'Inner'
    Outer o = (Outer){.inner = (Inner){.v = 7}};
```

because the field slot is `int64_t` while `make-struct` supplies a by-value
`Inner`.  So the read-back and the storage representation cannot both be made
consistent without changing how the field is stored.

## Root cause

`src/compiler/elab_structs.c`, the bare-symbol user-type fallthrough in both
defstruct field branches (new-style ~line 775, old-style ~line 885):

```c
if (tb && (tb->type.kind == TY_STRUCT || tb->type.kind == TY_ADT)) {
    fkind = TY_INT;     /* storage = int64 carrier */
    finner = TY_UNKNOWN;
    /* opaque newtypes now also record full_type (carrier-consistent);
       by-value struct/ADT fields deliberately do NOT, to avoid a
       byvalue-into-int64 make-struct mismatch. */
}
```

The storage decision (`TY_INT`) predates by-value struct support; it assumes
every user-typed field is a heap/opaque carrier (int64 pointer/handle).  A
by-value struct field needs `fkind = TY_STRUCT` (storing the struct inline)
*and* the nominal `full_type`, so both construction and access agree.

## Proposed fix directions

1. When the resolved field type is a by-value (non-heap, non-opaque) struct,
   store it by value: set `fkind = TY_STRUCT` (carry the `StructDef`) and
   record `full_type`.  `make-struct` then initializes the inline struct and
   `(.field x)` reads back the struct -- both consistent.  ADTs (carrier
   pointers) can keep the int64 storage but should still record `full_type`
   (the access path already treats ADT carriers as int64-consistent).
2. Audit the struct-layout / drop-glue / copy-kind paths for by-value struct
   fields (a non-copy by-value field makes the container non-copy, etc.).
3. Once landed, the `:int` self-link / nested-struct fields in
   `tests/fixtures/clone-list` and `typeclass-poly-wrapper-struct-receiver`
   can be tightened to their real types.

## Validation when fixed

- The minimal repro compiles and runs (`use-inner` returns 7) with no `:int`
  field and no `(:: ... :T)` re-pin.
- `bash tests/run.sh` green; regenerate snapshots only if struct-field
  storage codegen genuinely moves.

## Cross-references

- `docs/archive/defstruct-bare-user-type-field-reads-back-as-int-carrier.md`
  -- the nullary-opaque variant (RESOLVED); this is the by-value struct/ADT
  residual it scoped out.
- CLAUDE.md "No Lazy `:int` Stand-Ins" -- this storage gap is the upstream
  cause of several such stand-ins in the fixture tree.
