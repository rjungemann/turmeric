---
title: Garbage Collection
category: Compiler Internals
description: How memory is managed in Turmeric — reference counting, the Bacon-Rajan cycle collector, arenas, and what is (and isn't) GC-managed in the compiled and interpreted paths
---

# Garbage Collection in Turmeric

Turmeric does **not** have a single, tracing, always-on garbage collector.
Memory management is layered: most values live on the stack or in an arena,
`rc<T>` values are reference-counted, and a Bacon-Rajan cycle collector sits
on top of RC to reclaim cycles when it is turned on. In v1 the cycle
collector defaults to `GC_DISABLED` — the RC layer alone is doing the work.

This guide explains what the runtime does, what the compiler emits, what the
interpreter deliberately does not free, and where the seams are.

---

## The layers, at a glance

```
┌──────────────────────────────────────────────────────┐
│  Bacon-Rajan cycle collector    src/runtime/gc.c     │  opt-in, off by default
├──────────────────────────────────────────────────────┤
│  Reference counting (rc<T> / weak<T>)                │  always on for rc<T>
│                                 src/runtime/rc.c     │
├──────────────────────────────────────────────────────┤
│  Arenas (bump allocators)       src/runtime/arena.c  │  bulk-freed
├──────────────────────────────────────────────────────┤
│  Stack + plain malloc / free                         │  everything else
└──────────────────────────────────────────────────────┘
```

Only values whose type is `rc<T>` participate in RC or GC. Everything else
is stack-allocated, arena-allocated, or manually freed at a well-defined
lifetime boundary.

---

## What GC is used for

The short answer: **cycles in `rc<T>` graphs.** The longer answer breaks
down by what each layer manages.

### Reference counting handles the common case

The RC layer (`src/runtime/rc.h`, `src/runtime/rc.c`) manages every value
whose type is `rc<T>`. That includes:

- Boxed closures with captured `rc<T>` children.
- Cons cells reachable through `rc<Cons>`.
- Boxed results and options — `rc<Result<T,E>>`, `rc<Option<T>>`.
- Existentials packed under `RCK_EXISTENTIAL` / `RCEXP_RC`.
- Any user `defstruct` used behind `rc<T>` — its walker is registered so
  fields that are themselves `rc<T>` get traced.

The compiler emits `rc_strong_increment` on clone and `rc_strong_decrement`
on drop, with **last-use elision** so a value that flows straight into a
consumer is passed without a bump. When the strong count hits zero, the
control block is freed via a deferred queue (`rc_free_queue.c`) that
flattens deep drop chains into an iterative loop — this keeps `rc<Cons>` of
a long list from blowing the C stack.

### The cycle collector handles the leftover

RC alone leaks cycles. That is what the GC layer is for. It is a
Bacon-Rajan trial-deletion collector:

1. When a strong decrement lands on a still-live block that could be part
   of a cycle, the block is added to a **suspect buffer** and colored
   PURPLE (`gc.c` — `gc_on_strong_decrement`, `rc.c:184`).
2. A collection cycle runs `mark_phase` (`gc.c:242-293`) over the suspects:
   colors reset to WHITE, blocks reachable from real roots (any block with
   `strong_count > 0` that is not a suspect) go BLACK, and reachability is
   propagated by each block's `walk_fn`.
3. `trial_deletion_phase` (`gc.c:296-343`) frees anything still WHITE — by
   definition, unreachable cyclic garbage.

The important part for users: **the collector only sees what the walker
sees.** `RCK_STRUCT` blocks provide a `walk_fn` registered at
`rc_cb_alloc_struct` time (`emit_expr.c:5594-5656`), and existentials with
`RCEXP_RC` payloads are visible through the same mechanism (EXG5). Blocks
allocated with `RCK_OPAQUE` (raw pointers, C handles) are invisible to the
walker — cycles that route through them will not be reclaimed.

### What triggers a collection

- `GC_DISABLED` (default in v1, `gc.c:33`) — the whole `trial_deletion_phase`
  early-returns (`gc.c:347`). RC still runs; cycles just accumulate.
- `GC_MANUAL` — collection runs only when user code calls `(gc!)`.
- `GC_THRESHOLD` — collection runs when the suspect buffer reaches 128
  entries (forced at 4096).
- `GC_AUTO` (CG5, experimental) -- collection runs at **allocation
  checkpoints**, with no `(gc!)` call anywhere in the program. Two triggers:
  the candidate buffer reaching `GC_SUSPECT_THRESHOLD`, or
  `GC_AUTO_ALLOC_INTERVAL` allocations since the last collection. Behind
  `--enable=cycle-gc`, because it makes collection *timing* implicit -- pause
  behaviour changes without any call site showing it.

There is no background thread and no time-based sweep. Outside `GC_AUTO`, user
code drives collection.

Deliberately, the automatic trigger sits at allocation sites and **never** on
the refcount-decrement path: collecting out of `rc_strong_decrement` reenters
the collector while a caller is mid-mutation.

Runtime knobs surface as three compiler intrinsics wired in
`elab_memory.c`:

| Form           | Effect                          |
|----------------|---------------------------------|
| `(gc!)`        | Force one collection cycle now  |
| `(gc-enable!)` | Enable, defaulting the mode to `GC_MANUAL` |
| `(gc-disable!)`| Return to `GC_DISABLED`         |
| `(gc-auto!)`   | Enable in `GC_AUTO` -- collect automatically (needs `--enable=cycle-gc`) |

### Seeing what the collector did

Four readers, all plain counts, plus a per-collection trace on stderr:

| Form | Returns |
|------|---------|
| `(gc-collections)` | collections run so far |
| `(gc-objects-freed)` | control blocks reclaimed |
| `(gc-live-blocks)` | rc blocks currently registered |
| `(gc-candidate-high-water)` | peak candidate-buffer occupancy |

```sh
TUR_GC_TRACE=1 ./my-program
[gc] #1 mode=3 candidates=128 freed=128 live=128->0
```

The high-water is the one to watch: `gc-live-blocks` is instantaneous and sits
near zero right after a collection, so it tells you little on its own. A
high-water that stays at `GC_SUSPECT_THRESHOLD` means the collector is keeping
up; one that climbs means it is not.

`(gc-enable!)` defaults to `GC_MANUAL`, not `GC_THRESHOLD` -- collection still
happens only when you ask for it with `(gc!)`. Call `gc_set_mode(GC_THRESHOLD)`
for the suspect-count trigger; an explicit mode set before `(gc-enable!)`
survives it.

> **What a collection actually reclaims (updated 2026-07-25).** `(gc!)` now
> reclaims **live strong `rc<T>` cycles** as well as weak-zombie blocks. The
> collector runs real Bacon-Rajan trial deletion: a strong decrement that leaves
> the count > 0 buffers a candidate root, and collection subtracts the internal
> (in-cycle) edges on a scratch counter to find blocks referenced only from
> within the candidate subgraph. Measured: 192 bytes per two-node cycle -> 0.
>
> One caveat remains: the collector only sees what the walker sees, so a cycle
> routed through an `RCK_OPAQUE` handle is still not reclaimed. Collection is
> driven by user code -- `(gc!)`, or the suspect threshold under
> `(gc-enable!)` -- **unless** `(gc-auto!)` is in play, which collects at
> allocation checkpoints on its own (CG5, `--enable=cycle-gc`). Breaking cycles
> with `weak<T>`, as in Rust, remains valid and is still the only option with the
> collector disabled (the default). See
> `docs/archive/gc-cycle-collection-plan.md` (shipped) and
> `docs/upcoming/v1/gc-cycle-collection-followup-plan.md` (remaining).

---

## What is *not* GC-managed

Most things. This is the part people miss.

**Stack values.** Primitives — `:int`, `:float`, `:bool`, `:cstr`
literals — live on the stack. No RC, no GC.

**Arena-allocated values.** The elaborator's scratch data, the
interpreter's value pool, and per-pass working memory all live in bump
arenas (`arena.c`) that are freed en masse at the end of their scope.
Individual values inside an arena are never freed one at a time.

**Non-`rc` heap values.** A `defstruct` used by value or a raw `:ptr<T>`
returned from inline C is *not* on the RC path. If your inline-C code
`malloc`s, your inline-C code frees.

This includes **boxed sum-type payloads.** A `defdata` constructor with a
heap payload (`(Cons 1 rest)`, `(Full 7)`) emits a `ctor_*` `malloc`, but a
bare non-`rc<T>` box is never auto-freed on *any* path -- matching it and
consuming the payload does not drop the box. This is the memory model, not a
`match`-teardown bug: e.g. a plain `(defdata List (Nil) (Cons :int :List))`
built and matched leaks its `Cons` boxes by design. Opt a boxed value into
managed lifetime by wrapping it in `rc<T>`, or use the substructural
(`^linear`/`^unique`) path. The compiled program's *runtime* is not
leak-checked (only the `emit-c`/`build` codegen path is), so these leaks pass
the suite silently.

**Boxes without walkers.** `RCK_OPAQUE` blocks are RC-managed (they get
freed when the count hits zero) but the cycle collector cannot see through
them. Wrap C handles in `defopaque` and give them a proper drop, not a
walker, when they have no `rc<T>` children.

---

## Ownership across the stdlib

Where does that leave the standard library? A 2026-07-24 audit of all ~138
stdlib modules found that **stdlib builds no `rc<T>` cycles and uses `weak<T>`
nowhere -- because it barely reaches for shared ownership at all.** `rc<T>`
appears only in `stdlib/rc.tur` (the module that *defines* it) and the generated
`stdlib/docstrings.tur`; no other module constructs, stores, or imports one. The
library reaches its leak-free state by *sidestepping* shared mutable ownership,
via three strategies:

- **Persistent-immutable with structural sharing** -- `hamt`, `map`, `set`,
  `list`, `string`. Every update returns a new root; sharing is refcounted at the
  **C layer** (`tur_hamt_retain`, the string header's `rc` field), not via the
  Turmeric `rc<T>` type. An immutable DAG has no mutable back-edges, so no cycle
  can form.
- **By-value / single-owner mutable storage** -- `vec`, `mutmap`, `grid`, `ref`,
  `sized-*`. Each owns the one buffer it mutates; there are no shared handles and
  no back-pointers written into shared nodes.
- **Linear / affine opaque handles** -- `chan`, `future`, `taskgroup`, `mutex`,
  `reactor`, `net`, ... are `defopaque ... :linear` / `:affine`. Single ownership
  and exactly-once teardown are the deliberate alternative to refcounting.

**Relative to Rust, the stdlib is already at or beyond the ideal.** Rust's rule
is "use `Rc`/`Arc` only when ownership is genuinely shared, and break every cycle
with `Weak`." The stdlib clears that bar by rarely needing shared ownership in
the first place (Clojure/Haskell-style persistence plus linear types), so there
are no cycles to break and `weak<T>` is unused.

The one forward-looking caveat: a stored `rc<T>` field is what *would* enable a
cycle, and the collector cannot reclaim a live strong cycle today
(see the Known gaps below). To keep the property from regressing silently, the
`tur_stdlib_no_rc_cycles` ctest guard (`tests/check-stdlib-no-rc-cycles.sh`)
fails if a stdlib type annotation introduces `rc<...>` without an explicit
`rc-cycle-ok` review marker. The plan to surface a proper `weak<T>` escape-hatch
API in `rc.tur` -- for the day shared ownership *is* wanted -- is
`docs/upcoming/v1/stdlib-weak-ref-audit-plan.md`.

---

## The interpreter is different

The tree-walking interpreter (`turi_eval`, `src/turi/eval.c`) makes
deliberately different choices, and this trips people up.

**Closures are never freed *individually*.** Frames, bindings, and closures
are region-allocated from the env's value pool (`turi_val_alloc`,
`value.c:16-18`; `eval_frame_new`/`eval_frame_free`, `eval.c:424-451`), and the
per-object free is a deliberate no-op because a closure can capture a frame that
outlives its defining scope and the interpreter does not track which. The whole
pool is then reclaimed wholesale at `turi_env_free` (`env.c:334-336`) -- this is
region memory reclaimed at teardown, **not** memory abandoned to the OS. The
practical split: a short-lived process (fork-per-fixture, one-shot `tur build`)
reclaims everything at exit; a **long-lived env** (a REPL/kernel that never tears
down) grows the pool monotonically until teardown. That growth -- not a lost
pointer -- is the real caveat; incremental reclamation via opt-in scratch
promotion exists but is off by default (see
`docs/upcoming/v1/turi-interp-incremental-reclamation-plan.md`).

**Collections (Vec/Set/Map) backing buffers** are `calloc`/`malloc`'d outside
the value pool and are **not** reclaimed on scope exit unless user code calls
`vec-free` / `set-free` / `map-free`. They are, however, tracked at creation and
swept at teardown (`env.c:320-327`), so they no longer leak past process exit --
that historical bug is resolved (`docs/archive/history/interp-collections-never-freed.md`,
`docs/archive/history/turi-interp-collections-libturi-plan.md`). Reclaiming them
*mid-run* (drop-glue on scope exit) is still future work
(`docs/upcoming/v1/turi-interp-incremental-reclamation-plan.md`).

**Consequence for `bash tests/run.sh`.** The compiler/codegen path is
leak-clean and runs with LeakSanitizer enabled. The interpreter harnesses
(`run-turi.sh`, `run-flags.sh`) default to `ASAN_OPTIONS=detect_leaks=0` -- not
because memory is abandoned, but because the interpreter deliberately does not
free incrementally, so LSan on the interp path is noise rather than signal. See
`docs/archive/history/asan-debug-leaks-plan.md`.

---

## Codegen surface (what you'll see in `emit-c` output)

Every `rc<T>` operation lowers to a call in the runtime preamble:

- `rc_cb_alloc_struct(size, type_id, drop_fn, walk_fn)` — box a struct
  payload with a walker for cycle visibility.
- `rc_cb_alloc_kinded(size, kind, sub_kind, ...)` — box existentials
  (`RCK_EXISTENTIAL`, `RCEXP_RC`) so the walker can see the witness.
- `rc_strong_increment(cb)` / `rc_strong_decrement(cb)` — clone / drop.
  The elaborator elides the pair when a value is used exactly once.
- `rc_weak_increment(cb)` / `rc_weak_decrement(cb)` — for `weak<T>`.
- `rc_upgrade(cb)` — weak → `option<rc<T>>`.

If you are looking at generated C and want to know whether a value is
GC-visible, look for `walk_fn` in the `rc_cb_alloc_*` call. If it is
`NULL` (or the block is `RCK_OPAQUE`), the collector cannot trace through
it.

---

## Reading list

If you want to touch this code:

1. `src/runtime/rc.h` — control-block layout, kind tags (`RCK_*`,
   `RCEXP_*`). Start here; everything else assumes this vocabulary.
2. `src/runtime/rc.c` — increment / decrement, the deferred free queue
   hookup, `gc_on_strong_decrement` at line 184.
3. `src/runtime/gc.h` and `src/runtime/gc.c` — modes, suspect buffer,
   `mark_phase` (242-293), `trial_deletion_phase` (296-343).
4. `src/compiler/emit_expr.c` — where `rc_cb_alloc_*` calls are emitted
   (lines 5594-5656); this is how types acquire walkers.
5. `src/turi/eval.c` — the interpreter's opposing policy (region allocation +
   no-op per-object free, lines 424-451) and why it is deliberate.

For historical context:

- `docs/archive/history/existential-gc-plan.md` — original EXG1 RC-for-existentials plan.
- `docs/archive/existential-gc-followup-plan.md` — EXG4/5/6 (cross-scope
  ownership, cycle visibility, `:linear`), shipped 2026-05.
- `docs/archive/history/end-to-end-monomorphization-plan.md` — the longer-term
  ABI direction; the current hybrid carrier/by-value boxing is the seam RC
  and GC sit on.

---

## Which copy of the collector runs (updated 2026-07-25)

The collector was written twice: `src/runtime/{rc,gc,rc_free_queue}.c`, and a
hand-written replica emitted into every compiled program. Divergence between
them produced five bugs, so the replica is being retired. As of DEDUP-4b:

| build path | collector |
|---|---|
| `tur build` (executable) | **linked from `libturt_runtime.a`** -- the same code the interpreter runs |
| `tur build --shared` (.so) | the emitted replica (a `.so` must stay self-contained) |
| bare `tur emit-c` | the emitted replica (its output must be standalone C) |
| no runtime archive locatable, or `--runtime=source` | the emitted replica |

`TUR_RCGC_FROM_ARCHIVE=0` forces the replica back; `=1` takes the archive even
for bare `emit-c`, for a caller who links `libturt_runtime.a` themselves.
`tools/gc-copy-diff.py` reports what still differs between the two.

## Known gaps

- Cycle collection is off by default. When enabled it now reclaims live strong
  `rc<T>` cycles as well as weak-zombies (CG0--CG2, 2026-07-25; measured 192
  bytes per cycle -> 0, archived at
  `docs/archive/gc-strong-cycles-not-collected.md`). Remaining gaps: a cycle
  routed through an `RCK_OPAQUE` block is invisible to the walker, and there is
  no automatic trigger -- user code drives collection via `(gc!)` or the
  suspect threshold.
- Weak-pointer handling in `trial_deletion_phase` (`gc.c:332-341`) assumes
  no live weak pointers at collection time — see the comment there.
- The walker relies on per-block metadata registered at allocation. Runtime
  type reflection is not available; a block with no `walk_fn` is opaque to
  the collector no matter what it actually points to.
- Interpreter memory is reclaimed at env teardown, but a long-lived env
  (REPL/kernel) is not bounded incrementally -- the value pool and per-eval
  arenas grow until teardown. The plan to bound it is
  `docs/upcoming/v1/turi-interp-incremental-reclamation-plan.md`.
