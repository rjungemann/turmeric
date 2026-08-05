# `vec` and `map` cannot hold `rc<T>` at all

**RESOLVED 2026-07-26.** Item 3 -- the last open piece -- landed as
**`stdlib/rcvec.tur`**, the GC-visible flat vector this report's design
section sketched, built exactly as sketched: an `rc<RcVec>` handle whose
control block is allocated through `rc_cb_alloc_struct` with its own walk and
drop hooks (`tur_rcvec_walk` / `tur_rcvec_drop`), emitted into every program's
preamble beside the map rc shims so they bind to whichever collector copy the
program runs. Every slot is an rc control-block pointer, so the one generic
walker serves every element type -- no per-monomorphization glue. Ownership is
borrow-and-retain on `rcvec-push!` and mint-for-caller on `rcvec-get`, pinned
by strong-count assertions in `tests/fixtures/rcvec-holds-rc-values`; the
headline assertion, the same 50-cycle shape the vec arm of
`gc-blind-spot-cycle-through-vec` strands at 50 live, is reclaimed to **0
live** in `tests/fixtures/rcvec-cycle-is-collected` (also a control fixture in
`tests/run-gc-leak-gate.sh`, so the drop hook is ASan-checked with the
collector on and off).

What deliberately stays as-is:

- The **plain `Vec` / `Map` stay untraceable**, by design -- the "why the
  obvious fix is wrong" section below still holds: an unowned buffer cannot be
  traced safely. Their fixture arms keep pinning that cost.
- `weak<T>` in any collection stays rejected (no count to take).
- The Map release path from Turmeric (no `map-free`) remains the recorded
  persistent-map baseline, unchanged by this.

The original report follows.

---

**Severity:** medium (expressiveness hole; hard codegen error, not a miscompile)
**Status:** open, but narrowed to one thing. Items 1 and 2 are done for `vec`
and, as of 2026-07-26, for `map` as well (the map plan (a)--(d) below). What
remains is item 3 alone: making the container itself GC-visible so a cycle
routed through a `Vec` buffer or a HAMT node is *reclaimed*, not merely
refcount-correct. There is a working answer for users today -- build the
container out of rc cells (`stdlib/rcchain.tur`), which the walker already
traces with no new machinery -- so this is a performance/completeness gap, not
an expressiveness one.
**Found by:** CG7 (gc-cycle-collection-followup-plan), while trying to write the
`RCK_OPAQUE` blind-spot fixtures

## Summary

Storing a reference-counted value in either built-in collection fails in
codegen:

```turmeric
(defstruct S :move [tag : int])
(defn main [] : int
  (let [a (rc/of (make-struct S 1))
        v (vec-of (rc/clone a))]     ;; or (hamt-of :k (rc/clone a))
    (println (rc/strong-count a)))
  0)
```

```
tur: emit: invalid EX_REINTERPRET rc -> int
```

Both `vec-of` and `hamt-of` fail identically. The value type-checks -- the
failure is at emission, where the element is reinterpreted into the collection's
`int64_t` carrier and `rc<T>` has no such reinterpretation.

## Why it matters

Two separate reasons.

**1. It is a plain expressiveness hole.** "A vector of shared handles" is an
ordinary shape -- a scene graph's children, a connection pool, an observer list.
Today the workaround is to erase the `rc<T>` to a raw handle and manage the
counts by hand, which is exactly the kind of `:int` type-erasure this codebase
otherwise rules out.

**2. It silently narrows the GC's documented blind spot.**
[docs/guides/gc-guide.md](../../guides/gc-guide.md) says a cycle routed through an
`RCK_OPAQUE` payload -- "collection buffers such as a `vec` of `rc<T>`" -- is not
reclaimed, and the cycle-collection plan called for fixtures asserting that
non-collection. Those fixtures cannot be written: the shape does not compile.

So the blind spot is currently *narrower* than documented, but for a reason
nobody would want -- the hole is closed by rejection, not by tracing. If
collections ever do accept `rc<T>`, the blind spot opens up for real and the
fixtures become writable and necessary at the same moment.

Worth noting the neighbouring case is fine: a closure that **captures** an
`rc<T>` releases it correctly at scope exit (verified -- live block count returns
to 0 across a collection). The blind spot is not "anything opaque leaks."

## Root cause

Not fully traced. The error comes from the `EX_REINTERPRET` emission path
rejecting `rc -> int`; the collections store elements as an `int64_t` carrier,
so an `rc<T>` element needs either a real boxing step or an element-type-aware
storage path.

## Fix directions

Roughly in increasing order of work:

1. ~~**Diagnose it properly.**~~ **DONE 2026-07-25.** Rejected at check time
   with a span, instead of an `abort()` deep in codegen:

   ```
   vpush.tur:6:18: error: cannot store an owning value (rc) in a collection:
   elements go through an int64 carrier that cannot hold a reference the
   collection would have to own. Store a plain handle, or keep the value
   outside the collection
   ```

   The check sits where elaboration would build the carrier reinterpret
   (`elab_call.c`) -- the last point that still has a span. Two earlier guesses
   at the hook were wrong and are worth not repeating: the emit site has no
   diagnostic channel at all (no `diag_emit` exists in that layer), and the
   variadic rest-arg check never fires because `vec-of` is a *macro* expanding
   to `vec-push!` calls.

   Rejecting rather than passing the value through is deliberate: an
   unconverted rc would put a control-block pointer in a slot the collection
   does not own -- a leak or a double-free depending on which side drops it.

   **Known wart:** a `(vec-of ...)` call reports the span of the macro's own
   body in `stdlib/vec.tur` rather than the user's call site. A direct
   `(vec-push! v x)` reports the user's span correctly. That is the general
   macro-expansion span limitation, not specific to this check.

   Pinned by `tests/fixtures/errors/collection-rejects-owning-element`.

   ### Ascription side (closed 2026-07-25)

   The rules above run in `elab_call.c`, so they covered call arguments and
   results. `::` builds its own `EX_REINTERPRET` in `elab_types.c` and walked
   straight past them, which left the whole check one cast from defeat:

   ```turmeric
   (vec-push! v (:: a :int))      ;; a : rc<S>
   ```

   That type-checked, stored the bare control-block pointer in a slot nobody
   owned, and the refcounts were nonsense afterwards -- measured, a strong count
   that should have read 1 printed garbage.

   Two asymmetric rules now, because the directions are not equally bad:

   - **owning -> anything: rejected.** Erasing a counted handle to a machine
     integer is the `:int` type-erasure this codebase rules out, and there is no
     shape where it is what the author meant.
   - **anything -> owning: rejected except the literal `0`.** That one form is a
     null handle -- `stdlib/rcchain.tur`'s `rcchain-nil` is exactly it. Any other integer
     fabricates a control-block pointer out of arithmetic.

   Pinned by `tests/fixtures/errors/ascription-rejects-owning-erasure` and
   `.../ascription-rejects-fabricated-handle`. The full suite passed unchanged,
   so nothing in tree was relying on the hole.

2. ~~**Box the element.**~~ **DONE 2026-07-25, for `vec`.** A `Vec[rc<T>]`
   now owns a strong reference per slot. `map`/`hamt` still rejects.

   The carrier crossing was never a representation problem -- an `rc<T>` *is* a
   control-block pointer, so the bits fit an `int64_t` slot exactly. What was
   missing was an owner for the count. So each crossing is now classified at
   elaboration (`own_carry_for_arg` / `own_carry_for_result`,
   `src/compiler/elab_call.c`) as one of three things:

   | | meaning | sites |
   |---|---|---|
   | RETAIN | the crossing mints a new reference | `vec-push!`/`vec-set-o!` element, `vec-get` result |
   | BORROW | an existing reference moves; nothing is minted | `tur-vec-homog__`/`vec-empty-like__` witnesses, `vec-pop!` result |
   | REJECT | unaccounted -- diagnose | everything else, including `map`/`hamt` |

   The release side reuses the pattern already in place for wide by-value
   elements: `tur-vec-elem-rc?` is a new emit-time type query
   (`src/compiler/emit_expr.c`) that folds to 1 when the monomorphized element
   type is an `rc<T>`, and `vec-free` / `vec-set!` / `vec-drop-last!` thread it
   as **bit 1** of the `owned` flag their `-o` helpers already took for boxes
   (bit 0). So the release lands at exactly the three points a `Vec[Point]`
   frees its element boxes.

   **Both push shapes net +1**, which is the part worth knowing:

   ```turmeric
   (vec-push! v a)              ; BORROW: `a` keeps its count, the Vec takes its own
   (vec-push! v (rc/clone a))   ; the clone already minted one; the Vec takes THAT
   ```

   Retaining on top of an `(rc/of ...)` / `(rc/clone ...)` argument would leave
   the Vec holding two counts and releasing one, so those two forms are
   recognised as minting (`own_arg_mints_reference`) and taken as-is. Every
   other argument shape is treated as a borrow and retained -- the safe default
   of the two, since a surplus retain leaks where a missing one double-frees.

   A read hands the caller its **own** count rather than a borrow, so
   `(vec-get v 0)` is safe to let-bind and drop while the Vec keeps its slot.
   `vec-pop!` is the exception: it transfers the slot's count out, mirroring the
   ownership-transfer note already on it for boxed elements.

   Pinned by `tests/fixtures/rc-of-vec-element-ownership` (all assertions are
   strong counts -- printing the elements back would pass with the counts
   wrong), which produces identical output under `--interpret`, and by the
   repointed `tests/fixtures/errors/collection-rejects-owning-element` for the
   still-rejected `map`/`hamt` side.

   **Fallout worth recording:** widening the rejection path to `hamt` exposed
   two latent bugs. `elab_call.c` dereferenced `args[i]` after the carrier wrap
   returned NULL (a segfault on the way to reporting an error it had already
   reported), and `resolve_ctor_field` left `drop_inner_def` holding the arena's
   0xbe poison on every field that was not a nested owning aggregate -- harmless
   only for as long as the drop/walk glue emitter walked ctor 0 alone, which
   stopped being true when the sum-type glue fix widened it to every ctor.

   **Still open:** `map`/`hamt`, and `weak<T>` in any collection (there is no
   count to take, so a weak slot would be a bare pointer nobody owns).
### The `map`/`hamt` side (investigated 2026-07-25)

Two things in the earlier notes here were wrong, and both would misdirect
whoever picks this up.

**Wrong claim 1: "a persistent HAMT shares nodes between versions, so release
on removal is not well-defined without refcounted nodes."** The machinery
already exists and is in use. `src/runtime/hamt.c` carries refcounted value
boxes (`tur_hamt_box_key` -- a `[refcount|size][payload]` header -- with
`tur_hamt_box_retain` / `tur_hamt_box_release`), thread-local retain/release
hooks (`g_hamt_val_retain` / `g_hamt_val_release`) saved and restored around
every mutating op, and a deferred free of structurally-shared nodes that runs
those hooks as entries actually die. Bit 1 of the `owned` bitmask already turns
it on for wide by-value values. Structural sharing is a solved problem here, not
the blocker.

**The real blocker is which copy of the refcounter gets called.** `hamt.c` has
*zero* rc dependency today, and it must stay that way. It is precompiled into
`libturt_runtime.a`, so an `rc_strong_decrement` call written inside it would
always bind to the archive's `rc.c` -- correct under the default archive mode,
but wrong under `TUR_RCGC_FROM_ARCHIVE=0` and `--shared`, where the program
carries its own emitted replica and the two would be different collectors
operating on the same blocks. This is exactly why the `vec` side works: its
release lives in `stdlib/vec.tur` inline-C, which is *emitted into the program*
and therefore binds to whichever copy that program uses.

So the value ops have to be **supplied by the caller and stored on the map**,
never referenced from inside `hamt.c`. The code is already shaped for it: keys
do exactly this (`m->key_ops.release`), and the value side hardcodes
`tur_hamt_box_release` in `tur_hamt_free` only because there has only ever been
one value-box op. Making the value ops symmetric with the key ops is the
enabling refactor.

**DONE 2026-07-26.** Two earlier attempts at this were written and reverted
after a leak-count regression, and the notes they left behind said the refactor
was "not behaviour-preserving" and told the next reader to start at
`tur_hamt_free`. **Both of those conclusions were wrong.** The refactor is
behaviour-preserving; `tur_hamt_free` is innocent. What was actually wrong was
the earlier attempts' code, and -- just as much -- the measurement harness.

#### The harness was the confound

The first two passes rebuilt `tur` between measurements, because `hamt.c` is
compiled into the compiler. That put a full rebuild inside every experiment,
and a rebuild moves the number on its own (an earlier control had already shown
an `__attribute__((destructor))` shifting 7 objects to 9 while being
semantically inert). Building the probe with `--runtime=source` removes the
confound entirely: `src/runtime/hamt.c` is compiled straight into the test
program, so a `hamt.c` edit is measured with **no `tur` rebuild at all**.

```sh
TUR_CC_FLAGS="-O1 -std=c99 -w -fno-strict-aliasing -g -fsanitize=address" \
  ./build/tur build --runtime=source \
  tests/fixtures/map-move-typed-value/input.tur -o /tmp/probe
ASAN_OPTIONS=detect_leaks=1 /tmp/probe
```

Stable across repeat runs, which the earlier harness was not. (Its absolute
numbers differ from the 528/7 recorded below -- different flags -- so compare
within a harness, never across.)

#### The bisect

Three slices, each measured on its own:

| slice | leaked |
| --- | --- |
| pristine | 824 B / **9** objects |
| B1 -- add `val_ops` to `Hamt`, populate it, consume nothing | 888 B / **9** |
| B2 -- flip `tur_hamt_free` to read `m->val_ops.release` | 888 B / **9** |
| B3 -- caller-supplied ops installed via the new `_vo` entry points | 888 B / **9** |

The count never moves. The +64 bytes is four maps x 16 B of new field -- the
same benign struct growth an earlier control had already isolated.

**B2 is the one that matters**: `tur_hamt_free` was the single site the previous
note named as the prime suspect, and flipping it alone changes nothing.

#### Shape of the landed change

`_eq_o`'s signature is untouched, so nothing on the emitted side or in the
fixture snapshots moves. The ops-taking entry points are additive:

- `tur_hamt_val_ops { retain, release }` in `hamt.h`, mirroring
  `tur_hamt_key_ops` minus the comparator.
- `tur_hamt_set_eq_vo` / `tur_hamt_del_eq_vo` take the ops, install them as the
  thread-local value hooks for the duration of the op, and stamp them on the
  resulting map.
- `tur_hamt_set_eq_o` / `_del_eq_o` are now one-line wrappers passing
  `tur_hamt_box_val_ops()`, which is exactly what they hardcoded before.
- `tur_hamt_free` and both `hamt_copy` inherit sites read the stored ops.

#### Inertness had to be proved, not assumed

A bisect that only ever reports "no change" is equally consistent with the new
path being dead code. `tests/test_hamt_val_ops.c` (ctest target
`tur_hamt_val_ops`, via `tests/run-hamt-val-ops.sh`) closes that hole under
ASan/UBSan/LSan: it passes ops over plain stack storage, so if the boxed-value
refcount were still hardcoded it would be handed a non-box pointer and ASan
would fire on the spot. It asserts the caller's ops run and balance across
distinct-hash inserts, collision-chain copies (where the retain side actually
fires -- 28 retains / 36 releases), and maps derived by a later `set`/`del`;
that `owned == 0` leaves value lifetime untouched; and that legacy `_eq_o`
still frees boxed values through the box refcount.

#### A pre-existing leak found on the way

Writing that test surfaced an unrelated bug: `tur_hamt_set(NULL, ...)` allocated
an empty base map to read `count`/`key_ops` off and never freed it, so every
insert into a NULL base leaked a 64-byte root. Fixed in the same change (the
temp is freed before returning, except on the `return m` path where the temp
*is* the returned map). It does not move the fixture number, because `map-new`
hands in a real map rather than NULL -- it needed a caller that passes NULL to
show up at all.

Worth noting for its own sake: the baseline bytes are a pre-existing leak. The
fixtures never free their maps, and a persistent map that is never freed leaks
by design -- so the number is a *baseline to hold*, not a target to reach.

**Wrong claim 2 -- and this one sat in front of all of it:
`map-assoc` could not take a move-typed value at all. FIXED 2026-07-25.**

```
stdlib/map.tur:495: error [TUR-E0201]: cannot copy unique value '__tur_mv__'
   (+ (mk-owned? __tur_mk__) (* 2 (tur-wide-byval? __tur_mv__)))
```

The macro binds the value once and *uses* it twice -- as the value, and as the
argument to the emit-time type query `tur-wide-byval?` -- and a move-typed
binding may be used at most once. It goes unnoticed because the only fixture
covering boxed values, `map-multiword-struct-value`, declares its struct
`:copy`. Declaring the query's parameter `^borrow` does **not** relax it; the
check fires on the second use at the call site, not inside the callee.

`rc<T>` is move-only, so **every** rc value would have hit this before reaching
any of the HAMT work.

**The fix, and the order of it is the whole trick.** The query is a pure *type*
probe -- its body discards the argument -- so it now takes a **borrow**
`(& v)`, and `emit_expr.c` peels that borrow at the **expression** level. Not
the type level: `TY_REF_IMMUT` carries only the target's `TypeKind`, which would
lose the `AdtDef` that `type_is_wide_byval_adt` needs, silently folding the flag
to 0 and storing a wide value unboxed.

A borrow alone was **not** enough, and this is the part worth not re-deriving:
the move at the value's own argument position happens *first* in argument order,
so the borrow was a genuine use-after-move and the checker was right to reject
it. Binding the flag in the macro's `let`, before the value is handed over, is
what makes the borrow precede the move. The flag is a compile-time constant with
no side effects, so hoisting it changes no evaluation order that matters.

Pinned by `tests/fixtures/map-move-typed-value`, which asserts both sides of the
boxing decision -- a botched peel fails silently, folding to "narrow" and storing
a wide value inline. Two snapshots regenerated for the extra binding.

Order of work, then: ~~(a) let `map-assoc` accept a move-typed value~~;
~~(b) make the HAMT's value ops caller-supplied and stored, symmetric with the
key ops~~; ~~(c) thread bit 2 through `map.tur` passing the rc ops from the
emitted side~~; ~~(d) register the map insert/read sinks in `own_carry_for_arg`
/ `own_carry_for_result` alongside the `vec-*` entries~~. **All four done
2026-07-26.**

#### (c) + (d): a Map can hold rc<T> values

`(map-assoc m k some-rc)` used to be a hard error -- "cannot store an owning
value (rc) in a collection". It now works, taking a strong reference on insert
and handing the caller its own reference on read. `tests/fixtures/map-holds-rc-values`:

```
(rc/of 41) -> 1                          ; just the local
(map-assoc m 1 (rc/clone a)) -> 2        ; the map owns one
(map-get m 1) -> 3                       ; the read minted the caller one
  ... scope exit -> 2
(map-assoc m1 1 (rc/clone b)) -> a stays 2, b is 2
```

Three pieces:

- **`tur-rc-value?`** (`emit_expr.c`) folds to 1 at emit time when the value's
  monomorphized type is `rc<T>`, giving bit 2 of the `owned` flag. Like
  `tur-wide-byval?` it takes a **borrow**, so probing the type does not consume
  the move-typed value -- the same ordering trick step (a) needed.
- **Emitted rc shims.** `__tur_rc_val_retain` / `__tur_rc_val_release` are
  emitted into the program's preamble, adapting `rc_strong_increment` (returns
  `uint64_t`) and `rc_strong_decrement` (returns `bool`) to the `void (*)(void *)`
  the HAMT wants -- casting the function pointers instead would be UB. They are
  emitted rather than living in `hamt.c` for the reason above: `hamt.c` could
  only ever call the archive's collector.
- **Sink registration** (`elab_call.c`): `map-assoc-eq-o` arg 3 is
  `OWN_CARRY_RETAIN`, `map-get-eq-o`'s result likewise. The carrier bridge mints
  the reference, exactly as for `vec-push!`, so `map.tur`'s inline-C stores the
  incoming reference without retaining again -- one mint per insert, in one
  place.

Both rc/GC linkage modes were checked and agree: default archive mode, and
`TUR_RCGC_FROM_ARCHIVE=0 --runtime=source` (the emitted replica). That agreement
is the whole point of emitting the shims.

**The persistent-overwrite case is the one to be careful about.** Writing key 1
in `m2` must NOT release the value `m1` still holds -- a naive "release the value
being replaced" passes a non-persistent test and corrupts this one. The fixture
asserts it explicitly.

**What is still not exercised: the release path from Turmeric.** The runtime half
is wired (`tur_hamt_free` releases through the stored ops, covered by
`tur_hamt_val_ops`), but Turmeric has no `map-free`, so no map version is ever
dropped and no release ever runs in a compiled program. `map-dissoc` does not
thread the value bits at all, so a removal does not release either -- which is
*correct* for a persistent map (the older version still holds the entry) but
means the map's strong reference outlives the program. This is the same
"a persistent map that is never freed leaks by design" baseline recorded above,
now with rc references in it rather than just nodes.

3. **Make the collection walkable** so the cycle collector can trace through it
   (CG3 item 2). **Measured and blocked -- see below.** Item (2) opened the
   blind spot for real: a `Vec[rc<T>]` compiles today, so a cycle can now
   genuinely route through a collection buffer and go unreclaimed.

   The fixtures the gc-guide's blind-spot section always called for are finally
   writable, and `tests/fixtures/gc-blind-spot-cycle-through-vec` pins the cost.
   It builds the *same* two-reference cycle twice, differing only in whether the
   back-edge rides an rc field or a Vec slot:

   ```
   direct: a --.peer--> b --.peer--> a       -> freed, 0 live
   vec:    n --.kids--> Vec --slot--> n      -> 0 freed, 50 live
   ```

   The refcounts are correct on both sides -- item (2) sees to that. What is
   missing is **visibility**: a Vec's buffer is an ordinary malloc'd
   `{ data, len, cap }` header with no `walk_fn`, so `gc_each_child` enumerates
   nothing for the field and the collector never learns the edge exists.

   ### The blind spot is narrower than "collections"

   A third arm in the same fixture settles what the gap actually is. Build the
   identical cycle through a **user-defined container of rc cells** --

   ```turmeric
   (defstruct Cell  :move [item : rc<CNode> next : rc<Cell>])
   (defstruct CNode :move [kids : rc<Cell>])
   ```

   -- and the collector reclaims it completely: 0 live. That is a container by
   any reasonable reading, and it needs no new machinery at all, because its
   links are ordinary `rc` fields that the emitted walk glue already
   enumerates.

   So this is **not** a missing capability in the walker, and it is not
   "collections" as a category. It is one specific thing: a flat malloc'd
   buffer with no `walk_fn`. Two consequences worth separating:

   - **For users, today:** if you need a collection of `rc<T>` the collector can
     trace, build it from rc cells. It works now. The flat `Vec` is the case to
     avoid, and only when the elements can form a cycle.
   - **For the fix:** the walker side is already proven, so the remaining work is
     entirely about making the container itself GC-visible -- which is what the
     design below is.

   ### Shipped: `stdlib/rcchain.tur`

   That first consequence is now a stdlib module rather than advice. `RcChain`
   is a parametric chain of `rc<A>` -- `[item : rc<A> next : rc<RcChain>]` --
   with `rcchain-nil` / `-cons` / `-empty?` / `-head` / `-next`. Opt-in via
   `(load "stdlib/rcchain.tur")` rather than autoloaded: it is only useful with
   the collector on, and adding it to the autoload list would renumber every
   codegen snapshot for a module most programs never touch.

   It needs **no compiler or runtime change at all** -- no inline-C, no walker
   hook, no elab sink. Every link is an ordinary `rc` field that the emitted
   walk glue already enumerates. `tests/fixtures/rcchain-cycle-is-collected`
   builds the same node-holds-its-own-collection cycle the vec arm strands and
   reclaims all of it.

   The accessors take their chain `^borrow`, which is load-bearing rather than
   stylistic: `rc<T>` is move-only, so a by-value parameter would poison the
   caller's binding after a single read and make the chain unwalkable.

   The trade is the obvious one: O(n) indexing and one rc block per element,
   against O(1) and one buffer. A `Vec` of handles that cannot cycle is still
   the better default; `RcChain` is for when they can.

   ### Why the obvious fix is wrong

   The tempting change is three lines in `emit_adt_byval_walk_glue`
   (`src/compiler/emit_module.c`): when a ctor field's type is `(Vec rc<T>)`,
   emit a loop reporting each slot as a child. **Do not do this.** The sweep in
   `gc_collect_white` frees the entire white set together and never releases
   references *out* of it -- correct only when every path into a traced object
   is itself GC-visible. A `Vec` is shared by raw pointer, has no count of its
   own, and is freed by hand, so the collector cannot tell whether a second
   holder still points at the same buffer. Tracing through it would let the
   sweep free a block a live `Vec` still references: a leak traded for a
   use-after-free, which is the worse of the two.

   ### What would actually work

   The container has to become GC-visible before its contents can be traced.
   Concretely, an rc-managed vector allocated through `rc_cb_alloc_struct`
   (`src/runtime/rc.c:146`) with a `walk_fn` of its own. Every slot is already a
   control-block pointer, so **one generic walker serves every element type** --
   no per-monomorphization glue:

   ```c
   static void tur_rcvec_walk(void *value, RcWalkChildFn cb, void *ctx) {
       struct { int64_t *data; int64_t len; int64_t cap; } *v = value;
       for (int64_t i = 0; i < v->len; i++)
           if (v->data[i]) cb((RcControlBlock *)(intptr_t)v->data[i], ctx);
   }
   ```

   Its `drop_fn` releases each slot and frees the buffer, which is what makes
   the ownership question answerable in the first place. Sketch of the work:
   a new stdlib module holding the header layout and the two C hooks; the push
   and read sinks registered in `own_carry_for_arg` / `own_carry_for_result`
   alongside the `vec-*` entries; and the blind-spot fixture flipped from
   "50 live" to "0 live" once it lands.

   ### Wrinkle found while probing this, now fixed

   `(:: (vec-new) (Vec rc<S>))` followed by a push failed with `expected tyvar,
   got rc<S>`, while `(Vec S)` and `(Vec int)` were fine -- so the fault looked
   rc-specific rather than annotation-specific.

   `rc<T>`/`weak<T>`/`ref<T>`/`lref<T>` written in a type-application argument
   (or a `::` ascription, or an alias) resolved to a type variable literally
   **named** `"rc<S>"`. `rc_family_type_from_keyword_name` exists to stop
   exactly that, but only the defn param/return ladders in `elab_fns.c` called
   it; `type_expr_from_form` hooked `ptr<T>` and then fell through to the
   named-tyvar fallback. The annotation was accepted and silently mistyped, so
   the error surfaced later and blamed the argument for the annotation's
   mistake. Fixed by wiring the resolver in beside the `ptr<T>` hook at both
   fallback sites (bare symbol and keyword form); pinned by
   `tests/fixtures/rc-family-type-in-app-argument`.

   This does not retire the plain `Vec`. Manual, non-refcounted buffers stay the
   right default for element types with no shared ownership -- the rc-managed
   container is for the case that needs tracing, and its extra block per
   container is the price of being traceable.
