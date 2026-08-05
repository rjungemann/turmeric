---
title: Multi-word by-value struct/ADT elements in Vec/Set/Map (element boxing)
category: Codegen / runtime / typed collections -- frontier
description: The element buffer of every heap collection is int64[] (Vec) or a single void* slot (HAMT), and that is a locked decision for interpreter parity and float/cstr reinterpret. So a multi-word by-value struct/ADT element (a :copy struct wider than one word, or a payload-carrying ADT) cannot be stored. This plan boxes such elements -- heap copy + refcount, pointer in the slot -- reusing the existing boxed-key (WKC2) machinery, rather than the ruled-out typed element buffer. It depends on the v1 element-dispatch fix.
status: DONE (v2 frontier) -- Vec elements + Map values + Map keys/Set landed and working on BOTH the compiled and interpreter paths (incl. structural Eq/Show); full box lifecycle done (keys, Map values, Vec elements on free, vec-set! overwrite, vec-drop-last!) -- LSan-clean; only the inherent consume-and-drop vec-pop! carrier-ABI limitation remains (documented, out of scope). Re-verified 2026-07-19.
---

# Multi-word by-value elements need boxing, not a typed buffer

## Progress (2026-07-19)

Re-verified at HEAD: every deliverable in this plan has landed. All six
fixtures exist and are named as described -- `vec-multiword-struct-element`,
`vec-multiword-struct-eq`, `vec-multiword-struct-mutate`,
`map-multiword-struct-key`, `map-multiword-struct-value`, and
`set-multiword-struct-element`. The stdlib lifecycle plumbing is in place:
`tur-wide-byval?` (`stdlib/map.tur:485`, threaded into `map-assoc` via the
`owned` flag), `tur-vec-elem-wide?` / `vec-free-o` / `vec-set-o!` /
`vec-drop-last-o!` (`stdlib/vec.tur`), all documented in `stdlib/docstrings.tur`.
The three interpreter parity gaps recorded in the 2026-07-12 note are all
resolved. The only residual is the inherent, documented consume-and-drop
`(:: (vec-pop! v) T)` carrier-ABI leak (`docs/archive/vec-set-pop-element-box-leak.md`),
which is out of scope for this plan. Nothing tracked here remains open --
ready to archive.

## Progress (2026-07-12)

Landed all three plan targets: **Vec elements**, **Map values**, and the **key**
case (**Map keys / Set elements**), each working on BOTH the compiled and
interpreter paths -- including structural `Eq`/`Show`. The interpreter parity
gaps are all closed (see the resolved list below). Box lifecycle is now mostly
done: the compiled `map-free` / `set-free` were shallow (they `free`d only the
`{void* hamt}` wrapper, leaking the whole HAMT + every owned key box) while the
interpreter's natives already deep-freed; they now call `tur_hamt_free` too, so a
`Map[Point int]` / `Set[Point]` with `mk-owned? = 1` struct keys frees the HAMT
nodes AND releases each boxed key exactly once (verified LSan-clean; refcount-safe
under structural sharing). **Map VALUE boxes are now released too**: a multi-word
struct value is boxed via `tur_hamt_box_key` and the HAMT retains/releases it
symmetrically with an owned key, gated by bit 1 of the `owned` flag that
`map-assoc` threads via `(tur-wide-byval? v)` (an emit-time type query folded per
monomorphization; the interpreter's pure-Turmeric fallback returns 0 since it
never C-boxes values).  `map-free` frees each boxed value exactly once
(LSan-clean, refcount-safe under structural sharing and key-update).  **Vec
element boxes are now released too**: `vec-free` is a macro forwarding
`(vec-free-o v (tur-vec-elem-wide? v))`, where `tur-vec-elem-wide?` is an
emit-time query (twin of `tur-wide-byval?`) that folds to 1 for a wide by-value
element; `vec-free-o` then `free`s each owned `data[i]` box before the buffer (a
Vec is unshared, so it owns its element boxes outright).  Verified LSan-clean; the
interpreter's `native_vec_free_o` frees the buffer + header (elements ride as
TuriStruct pointers it owns separately).  The mutation paths that orphaned a box
are handled too: `vec-set!` (macro -> `vec-set-o!`) frees the overwritten slot's
old box, and a new `vec-drop-last!` (macro -> `vec-drop-last-o!`) removes+frees
the last box for the discard case.  The one residual is inherent, not a
collection leak: a consume-and-drop `(:: (vec-pop! v) T)` on a wide element leaks
that box, because reading a > 8 byte value out of the carrier needs a live box
the ascription derefs and nothing owns it afterward -- callers remove-and-inspect
via `vec-get` + `vec-drop-last!` instead (documented in the archived report
`docs/archive/vec-set-pop-element-box-leak.md`).

**Done -- Vec multi-word elements (compiled + interpreter).**
`(vec-of (Point 1 2) (Point 3 4))` for `(defstruct Point :copy [x : int y : int])`
now stores each element heap-boxed (malloc + copy: value semantics, the Vec owns
its copy) with the box pointer on the int64 carrier; `vec-get` reinterprets the
carrier as the box pointer and loads the aggregate by value. Field projection
(`.x`), `Show[Vec]`, and `Eq[Vec]` all recover the concrete element through the
carrier. The fix is entirely in the carrier-bridge dispatch (`emit_expr.c`),
keyed on `type_is_wide_byval_adt` (the pre-existing ">8 byte by-value ADT"
predicate) -- the element buffer stays `int64[]` as the ABI matrix requires.
Interpreter parity is free: `native_vec_*` already boxes a struct element as a
`TURI_STRUCT` pointer (`vec_retag_cell`), so field-projection and `Show` agree
bit-for-bit. Fixtures: `vec-multiword-struct-element` (both paths),
`vec-multiword-struct-eq` (compiled-only; see interpreter gap below).

**Done -- Map multi-word values (compiled).**
`(map-assoc (:: (map-new) (Map int Point)) 1 (Point 7 8))` heap-boxes the value
onto the carrier through `map-assoc-eq-o`'s inline-C `val : V` param; the
`(:: (map-get m k) Point)` read derefs the box. Fixture:
`map-multiword-struct-value` (compiled-only).

**The four codegen seams (all in `src/compiler/emit_expr.c`):**
1. *Vec push (store).* The `!matched_spec` bare-TY_ADT-byvalue -> tyvar-carrier
   block routes a WIDE by-value element through `emit_carrier_bridge_escaping`
   (heap-promote) instead of the stack-spill bridge (which dangled).
2. *Map value push (store).* A sibling block for the `matched_spec` + inline-C
   carrier insert, gated on the spec's param actually lowering to `int64_t` (some
   inline-C specs specialize the param to the concrete aggregate -- boxing there
   is a type error; that guard is load-bearing, it fixed the `dense_set`
   regression).
3. *Get + field access (read).* `EX_GET_FIELD` detects a carrier-producing
   wide-byval accessor receiver (`recv_call_carrier_byval`) and routes it into
   the existing deref path.
4. *Get + method/fn arg (read).* A carrier->concrete deref-unbox for a wide-byval
   arg fed to a concrete param (via `reresolved_callee->param_types[i]` for a
   re-dispatched method, or the arg's own recovered type for a direct concrete
   instance method); plus the `fn_body_tail_byvalue_carrier_type` recovery no
   longer excludes a concrete-layout aggregate when the accessor's result is a
   bare tyvar.

**Done -- Map multi-word keys and Set multi-word elements (BOTH paths).**
A multi-word Map KEY (`(Map Point int)`) and Set element are content-keyed
through the existing WKC2 boxed-key path, on the compiled AND interpreter paths,
via `#?(:tur ... :turi ...)` reader conditionals in the three `Hash`/`MapKey`
instance methods.

- **Compiled (`:tur`).** `mk-box` copies the key into a `tur_hamt_box_key`
  refcount box (from its monomorphic concrete param `p`, so `&p`/`sizeof(p)` are
  the concrete struct), `mk-owned? = 1` retains/releases the box across
  structural sharing (freed exactly once -- **LSan clean**, KEY lifecycle
  deliverable met), and `mk-cmp` returns the **generic size-aware comparator**
  `tur_hamt_box_key_eq` (`src/runtime/hamt.c`, exposed via `hamt.tur`): it reads
  each boxed payload's length from the box header and byte-compares, so a SINGLE
  comparator serves every multi-word key type -- removing the per-struct
  comparator + C-type-name boilerplate the earlier `eqmap-struct-content`
  fixture carried.
- **Interpreter (`:turi`).** The tree-walking interpreter stores the struct key
  as a `TuriStruct` value; the `:turi` bodies are uniform (no per-field code):
  `hash` -> `(struct-hash p)`, `mk-box` -> `p`, `mk-cmp` -> `(struct-key-cmp)`,
  `mk-owned?` -> `0`. `struct-hash` / `struct-key-cmp` are new generic
  interpreter natives (`turi_struct_hash_c` / `turi_struct_key_eq_c` in
  `src/turi/eval.c`) -- the interpreter analogues of xxh64 / `tur_hamt_box_key_eq`
  that fold/compare a `TuriStruct`'s field content recursively. Crucially
  `struct-key-cmp` returns a **stampable C fn pointer** (not a Turmeric closure),
  exactly like the primitive `cstr`/`float` `mk-cmp` natives, so it is stamped on
  the HAMT root by the `_eq_o` path and RECOVERED by `tur_hamt_keyeq` -- which is
  what makes structural `Eq[Map]` / `Eq[Set]` over struct keys agree with the
  compiled path, not just `assoc`/`get`/`member`. The set `_eq_o` interpreter
  natives (`native_set_{add,has,del}_eq_o`) also gained the closure-comparator
  trampoline the map natives already had, so a struct comparator never wild-jumps
  there.

Set needed no stdlib work: it retyped to `:A` with `MapKey[A]` at the v1 Set
generalization, so a struct element is just a boxed key. Fixtures
`map-multiword-struct-key` / `set-multiword-struct-element` now run on BOTH paths
(whitelisted in `run-turi.sh` `TURI_INLINEC_RUN`) and agree bit-for-bit,
including structural `Eq`.

The three `:turi` bodies are generic, so only `Eq[Point]` (which the user writes
anyway) mentions fields. A `derive-struct-key` MACRO to stamp the whole
instance-set per struct is still not feasible (a `defmacro` cannot template the
`:tur` inline-C block -- a string body emits as a returned `const char *`, not a
C block), so the ~10-line reader-conditional pattern is the interface; the
generic runtime + interpreter comparators/hashes keep it type-name-free and
per-field-free.

**Interpreter parity gaps -- ALL RESOLVED.** Every multi-word-struct collection
case now runs on BOTH the compiled and interpreter paths, bit-for-bit:
- (RESOLVED) Map struct KEYS / Set struct elements: `#?(:tur ... :turi ...)`
  reader conditionals let the interpreter run pure-Turmeric `:turi` bodies backed
  by the `struct-hash` / `struct-key-cmp` natives, including structural `Eq`.
- (RESOLVED) `Eq[Vec]` / `Show[Vec]` over a struct element: the interpreter's
  generic-dict-dispatch re-resolver (`gde_reresolve_method`, `eval.c`) pointer-
  matched instances, but the auto-loaded `vec-eq-loop`/`vec-show-loop` bake a
  `dict_arg` under a DUPLICATE `Eq`/`Show` typeclass object (the class is
  declared in the auto-loaded stub AND again when the program loads
  typeclass.tur), so no instance matched and it kept the carrier `Eq[int]`,
  pointer-comparing the elements.  The re-resolver now falls back to matching a
  class's instances BY NAME for a non-primitive concrete (primitives keep their
  exact prior dispatch), so `Eq[Point]`/`Show[Point]` are threaded.
  (`vec-multiword-struct-eq` now runs on both paths.)
- (RESOLVED) Map struct VALUES: the value rides the int64 carrier as a
  `TuriStruct` pointer, which `get_field_extract` read as a compiled raw int64[]
  field buffer.  `(:: (map-get m k) T)` now retags that carrier to `TURI_STRUCT`
  (`try_retag_carrier_struct`, `eval.c`), gated on a multi-word (>= 2 field)
  single-ctor record so a `defopaque` int newtype / single-word value is never
  dereferenced as a pointer.  (`map-multiword-struct-value` now runs on both
  paths.)

**Lifecycle:** the KEY / Set-element box lifecycle is done (`mk-owned? = 1` ->
`tur_hamt_box_retain`/`release`), and the compiled `map-free`/`set-free` now
`tur_hamt_free` the backing HAMT (previously shallow -- a `free(wrapper)` that
leaked the HAMT and every owned key), so the key boxes are released exactly once
and LSan-clean when the collection is freed.  **Map VALUE boxes are released
too** now: bit 1 of the `owned` flag installs a symmetric value-box refcount on
the HAMT (`g_hamt_val_retain`/`release` in `src/runtime/hamt.c`), the map-value
escaping bridge boxes via `tur_hamt_box_key`, and `map-assoc` threads the bit via
`(tur-wide-byval? v)`.  **Vec element boxes are released too** now: `vec-free` is
a macro that forwards `v` to `vec-free-o` along with `(tur-vec-elem-wide? v)` --
an emit-time query (twin of `tur-wide-byval?`) that peels the element type out of
the `(Vec A)` spine and folds to 1 for a wide by-value `A`.  Since a Vec is
mutable and NOT structurally shared it owns its element boxes outright, so
`vec-free-o` `free`s each `data[i]` box before the buffer (verified LSan-clean;
the only residual on the repro is the unrelated process-lifetime `show` string
allocs).  The interpreter's `native_vec_free_o` ignores the flag (elements ride
as TuriStruct pointers it owns separately).  `vec-set!` (macro -> `vec-set-o!`)
frees the overwritten slot's old box, and `vec-drop-last!` (macro ->
`vec-drop-last-o!`) removes+frees the last box for the discard case; the only
residual is the inherent consume-and-drop `(:: (vec-pop! v) T)` carrier-ABI leak
(see `docs/archive/vec-set-pop-element-box-leak.md`), plus the intermediate
persistent-map versions every `map-assoc` mints -- all process-lifetime.  The
compiled fixtures run under the default leak-detection-off program policy.

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
   value. `vec-free` releases (**landed**): it is a macro forwarding
   `(vec-free-o v (tur-vec-elem-wide? v))`, and `vec-free-o` `free`s each owned
   `data[i]` box before the buffer when the flag is set.  As anticipated below,
   the actual boxes are a **plain `malloc` owned pointer**, not the refcount box
   -- because a Vec is mutable and not structurally shared, it owns its element
   boxes outright, so `vec-free` frees them directly (no retain/release).  The
   element-removal cases are handled too: `vec-set!` frees the overwritten box
   and `vec-drop-last!` frees the removed box (both landed); only the inherent
   consume-and-drop `(:: (vec-pop! v) T)` carrier-ABI leak remains, documented in
   `docs/archive/vec-set-pop-element-box-leak.md`.  A deep `vec-clone` (not yet
   present) would need to copy each box.
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
- Persistence: a structurally-shared HAMT with boxed multi-word KEYS frees each
  key box exactly once on `map-free`/`set-free` (LSan-clean, refcount-safe under
  sharing).  Boxed multi-word VALUES are released too (symmetric HAMT value-box
  refcount), and Vec element boxes are released on `vec-free` (plain owned
  pointer, unshared); `vec-set!` frees the overwritten box and `vec-drop-last!`
  the removed box.  The only residual is the inherent consume-and-drop
  `(:: (vec-pop! v) T)` carrier-ABI leak
  (`docs/archive/vec-set-pop-element-box-leak.md`).
  NB: the compiled test harness does not run programs under LSan, so this is
  verified by hand (emit-c + `-fsanitize=address` + `libturi.a`), not by a
  fixture.

## Related

- `docs/artifacts/parametric-type-abi-matrix.md:108-141` -- the locked
  "element buffer stays int64" frontier and why typed buffers are out.
- `src/runtime/hamt.h` (WKC2/WKC3) -- the boxed-key refcount machinery to
  generalize.
- `docs/upcoming/v1/containers-eq-show-element-dispatch-plan.md` -- the hard
  prerequisite (type recovery on element read).
- `docs/upcoming/v1/set-element-api-generalization-plan.md` -- Set's `:A`
  retype, after which multi-word Set elements are just boxed keys.
