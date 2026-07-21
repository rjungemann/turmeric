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

There is no background thread, no time-based sweep, no allocation-count
heuristic beyond the suspect threshold. User code drives collection.

Runtime knobs surface as three compiler intrinsics wired in
`elab_memory.c`:

| Form           | Effect                          |
|----------------|---------------------------------|
| `(gc!)`        | Force one collection cycle now  |
| `(gc-enable!)` | Switch to `GC_THRESHOLD`        |
| `(gc-disable!)`| Return to `GC_DISABLED`         |

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

## The interpreter is different

The tree-walking interpreter (`turi_eval`, `src/turi/eval.c`) makes
deliberately different choices, and this trips people up.

**Closures are never freed.** `eval.c:435-443` documents the reason:
frames may be captured by closures that outlive their defining scope, and
the interpreter does not try to figure out which. Process-lifetime is fine
for the REPL and for compilation, which is what the interpreter is for.

**Collections (Vec/Set/Map) are never freed** unless user code calls
`vec-free` / `set-free` / `map-free` explicitly. The natives in `main.c`
`calloc` their backing storage and never register it for cleanup. See
`docs/reported/interp-collections-never-freed.md` for the active report and
`docs/upcoming/turi-interp-collections-libturi-plan.md` for the plan to
move those natives into `libturi.a` where the embedder can wrap them.

**Consequence for `bash tests/run.sh`.** The compiler/codegen path is
leak-clean and runs with LeakSanitizer enabled. The interpreter harnesses
(`run-turi.sh`, `run-flags.sh`) default to `ASAN_OPTIONS=detect_leaks=0`
because a genuine leak in the interpreter is expected. See
`docs/asan-debug-leaks-plan.md`.

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
5. `src/turi/eval.c` — the interpreter's opposing policy (lines 435-443,
   318-324) and why it is deliberate.

For historical context:

- `docs/archive/history/existential-gc-plan.md` — original EXG1 RC-for-existentials plan.
- `docs/archive/existential-gc-followup-plan.md` — EXG4/5/6 (cross-scope
  ownership, cycle visibility, `:linear`), shipped 2026-05.
- `docs/upcoming/end-to-end-monomorphization-plan.md` — the longer-term
  ABI direction; the current hybrid carrier/by-value boxing is the seam RC
  and GC sit on.

---

## Known gaps

- Cycle collection is off by default. Programs that build cycles need to
  call `(gc!)` explicitly, or opt into `GC_THRESHOLD` via `(gc-enable!)`.
- Weak-pointer handling in `trial_deletion_phase` (`gc.c:332-341`) assumes
  no live weak pointers at collection time — see the comment there.
- The walker relies on per-block metadata registered at allocation. Runtime
  type reflection is not available; a block with no `walk_fn` is opaque to
  the collector no matter what it actually points to.
- Interpreter collections leak until the process exits. This is by design
  today; the plan to fix it is `turi-interp-collections-libturi-plan.md`.
