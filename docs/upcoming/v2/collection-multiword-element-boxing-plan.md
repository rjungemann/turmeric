---
title: Multi-word by-value struct/ADT elements in Vec/Set/Map (element boxing)
category: Codegen / runtime / typed collections -- frontier
description: The element buffer of every heap collection is int64[] (Vec) or a single void* slot (HAMT), and that is a locked decision for interpreter parity and float/cstr reinterpret. So a multi-word by-value struct/ADT element (a :copy struct wider than one word, or a payload-carrying ADT) cannot be stored. This plan boxes such elements -- heap copy + refcount, pointer in the slot -- reusing the existing boxed-key (WKC2) machinery, rather than the ruled-out typed element buffer. It depends on the v1 element-dispatch fix.
status: proposed (v2 frontier)
---

# Multi-word by-value elements need boxing, not a typed buffer

## The problem, and why it is not just the dispatch bug

Verified at HEAD: `(vec-of (Point 1 2) (Point 3 4))` for
`(defstruct Point :copy [x : int y : int])` then `(.x (vec-get v 0))` fails.
So does a `:heap` `Box` element at `(.v (vec-get v 1))`. The `:heap` case is the
v1 element-**dispatch** bug (a `:heap` handle is one pointer word -- it *stores*
fine; the read just can't recover its type to project a field). The `:copy`
`Point` case is **worse**: a two-`int` struct is 16 bytes and does not fit the
single int64 element slot at all -- it is a **storage** problem on top of the
dispatch problem.

Element categories, by whether they fit one machine word:

| Element kind | Fits int64 slot? | Status |
|---|---|---|
| Primitive (`int`/`bool`/`float`/...) | yes (reinterpret for float) | stored; dispatch is v1 |
| `cstr` | yes (the `char*`) | stored; dispatch is v1 |
| Opaque handle / `:heap` struct / another collection | yes (the pointer) | stored; dispatch is v1 |
| **`:copy` struct wider than one word** | **no** | **this plan** |
| **Payload-carrying ADT wider than one word** | **no** | **this plan** |

So this plan is specifically the last two rows: **by-value aggregates too wide
for the carrier slot.**

## Why the "obvious" fix is ruled out

Typing the element buffer per element type (`Point[] data` for `Vec[Point]`,
`vec-get` returning `Point` by value) is **explicitly rejected** in
`docs/artifacts/parametric-type-abi-matrix.md:108-141` ("The element buffer
stays int64 -- the typed-pointer decision is HEADER-only"):

- the interpreter's `native_vec_*` walk the same `int64[]` buffer; a typed
  element buffer would **fork compiled vs interpreted storage and break parity**;
- `float`/`cstr` elements are *bit*-reinterpreted out of the int64 slot; a typed
  `double[]`/`const char*[]` buffer changes those reads.

The matrix calls element-buffer monomorphization "a separate, deep frontier ...
not scheduled ... needs its own design pass." This plan is that design pass, and
it deliberately does **not** type the buffer.

## Approach: box the aggregate, keep the buffer int64

Store a **heap-boxed copy** of a multi-word element and put the box's payload
pointer in the int64 slot. The slot stays one word; the buffer stays `int64[]`;
parity and float reads are untouched. Reading an element reinterprets the slot
as the box pointer, then derefs/loads the aggregate by value.

This is exactly the **WKC2 boxed-key mechanism generalized from keys to
elements/values.** `src/runtime/hamt.h` already ships:

- `tur_hamt_box_key(src, n)` -- heap box holding a refcount + `n` copied bytes,
  returns the payload pointer;
- `tur_hamt_box_retain` / `tur_hamt_box_release` -- refcount ops so a single box
  is shared safely across a persistent structure's structural sharing;
- `tur_hamt_box_key_ops()` + the `_eq_owned` / `owned=1` op family that retains
  on structural copy and releases on entry drop.

Multi-word **Map keys already use this** (the `owned` flag in `map-assoc`). This
plan extends the same refcount-box discipline to:

1. **Vec elements.** `vec-push!` of a multi-word `A` boxes the value (copy `n`
   bytes) and stores the payload pointer; `vec-get` reinterprets and loads by
   value. `vec-free` / element removal releases. A copied Vec (`vec-clone`)
   retains each box. Because Vec is mutable and not structurally shared, refcount
   could in principle be a plain owned pointer, but reusing the refcount box
   keeps one code path and makes `Vec` values that escape into a shared
   structure safe.
2. **Map values.** Today a `:V` value rides the int64 carrier (single word). A
   multi-word by-value struct value boxes exactly like a key does, via a
   value-side `owned` flag threaded into the entry (the HAMT entry's `val` is a
   `void*` -- point it at a value box; extend the ownership ops to retain/release
   the value box symmetrically with the key box).
3. **Set elements.** After the v1 Set generalization plan retypes Set to `:A`
   with `MapKey[A]`, a multi-word Set element is just a boxed key -- it falls out
   of the same `MapKey`/`owned=1` path with no extra work.

## What the compiler must decide

- **When to box.** Only for an element/value/key whose resolved type is a
  multi-word by-value aggregate (`CK_CONCRETE` struct/ADT wider than 8 bytes).
  Single-word types (all primitives, `cstr`, opaque handles, `:heap` structs,
  nested collections) keep riding the raw carrier -- no boxing, no regression.
  The decision is a size/kind check on the monomorphized element type at the
  `vec-push!` / `map-assoc` / `set-add` spec site.
- **The push/get ABI.** `vec-push! [A] [v val : A]` at a multi-word `A` spec
  must box `val` (spill to a temp, `memcpy` into a fresh box, store the pointer);
  `vec-get [A] : A` at that spec must load the aggregate back by value from the
  box. This composes with the v1 element-dispatch work (the spec already grounds
  `A` to the concrete struct; add the box/unbox at the boundary).
- **Ownership semantics.** Boxing copies -- inserting a struct does not alias the
  caller's value; the collection owns its box. This is the correct value
  semantics for a `:copy` element and matches how `:copy` structs behave
  elsewhere. Document it.

## Dependencies and sequencing

- **Hard prerequisite: the v1 element-dispatch fix**
  (`containers-eq-show-element-dispatch-plan.md`). Even a correctly *stored*
  boxed struct is useless if the read-back cannot recover its type to project a
  field or dispatch `Eq`/`Show`. Do v1 first.
- **Interpreter parity is a first-class requirement, not an afterthought.** The
  matrix's whole reason for the int64 buffer is parity. `native_vec_*` /
  `native_map_*` must gain the identical box/unbox behavior (allocate a box,
  memcpy the interpreter's value bytes, store the pointer; reverse on read) so
  compiled and interpreted programs agree bit-for-bit. Budget real work here.
- **Float-containing structs are fine** -- boxing copies raw bytes, so a struct
  with `float` fields round-trips; only the *bare* `float` element case relies on
  the reinterpret, and bare `float` is single-word (not boxed).

## Explicitly out of scope

- Typed element buffers / element-buffer monomorphization (ruled out above).
- Bare-`float`/`cstr` element storage changes (they are single-word; untouched).
- Zero-copy / arena element storage -- a later optimization, not correctness.

## Tests (deliverable)

- `Vec[Point]` (`:copy`, multi-word): push, `vec-get`, project a field,
  `Eq`/`Show` (once v1 lands) -- value semantics (mutating the source after
  insert does not change the stored element).
- `Map[K Point]` (multi-word **value**) and `Map[Point V]` (multi-word key,
  already partly supported) round-trip + structural `Eq`.
- `Set[Point]` membership + content dedup.
- Compiled vs `--interpret` parity fixtures for each (identical output).
- Persistence: a structurally-shared HAMT with boxed multi-word entries frees
  each box exactly once (LSan clean).

## Related

- `docs/artifacts/parametric-type-abi-matrix.md:108-141` -- the locked
  "element buffer stays int64" frontier and why typed buffers are out.
- `src/runtime/hamt.h` (WKC2/WKC3) -- the boxed-key refcount machinery to
  generalize.
- `docs/upcoming/v1/containers-eq-show-element-dispatch-plan.md` -- the hard
  prerequisite (type recovery on element read).
- `docs/upcoming/v1/set-element-api-generalization-plan.md` -- Set's `:A`
  retype, after which multi-word Set elements are just boxed keys.
