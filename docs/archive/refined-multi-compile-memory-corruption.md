# Compiling multiple refined files in one process corrupts memory (nondeterministic SIGSEGV)

**RESOLVED (2026-07-26).** Root cause: NOT an arena use-after-free after all --
`parse_typeclass_method` (`src/compiler/elab_typeclasses.c`) never initialized
the RT1 memo field `TypeClassMethod.refine_class_binding`, and `arena_alloc`
does not zero. A fresh process's first compile reads the field from zero
OS pages (NULL -> works); later in-process compiles read recycled malloc slab
junk (the previous compile's bytes), so `rt_class_method_refine_binding`'s
memo check returned a garbage `Binding*` that `refine_note_call_site`
dereferenced. Fixed by zeroing the struct after allocation. Found by executing
`docs/archive/arena-debug-poisoning-plan.md`: the AP4 guard mode
(`TUR_DEBUG_ARENA_GUARD=1`, mmap + PROT_NONE on free) made the deterministic
8/8 repro go CLEAN -- impossible for a real UAF, and exactly what an
uninitialized read does when fresh mappings are zero-filled. Validated: spices
`ecs/tests/refined` repro 0/20 failures (was 8/8 SIGSEGV), the minimal 2-file
repro below 0/10, refine fixtures green under a Debug tur. The RE1 refined ecs
tests can now auto-run via `tur test`.

**Severity:** high (blocks auto-running refined tests via `tur test`, and any
multi-file in-process refined compile -- LSP/worker). Nondeterministic, so it can
also flake CI. Individual `tur check`/`run`/`emit-c` (one file per process) are
unaffected.

## Summary

`tur test <dir>` compiles every test file in ONE process (sequential
`cmd_build` calls). When two or more of those files use refinements
(`#lang turmeric refined` / `--enable=refined`), the process **segfaults
nondeterministically** -- the first file usually compiles and runs (prints its
output), and a subsequent refined `cmd_build` crashes. Measured crash rates on
macOS/Darwin (Debug build): 6-8 out of 8 runs on a set of 3-4 refined files;
the SAME set has also passed cleanly (nondeterministic). A single-file
`tur check`/`run` on each of those files always succeeds.

## Reproduce

Any two refined files in a directory, run through `tur test`:

```sh
mkdir t
cat > t/a.tur <<'EOF'
#lang turmeric refined
(defmodule a (export)
(defstruct World [n : int])
(defn alive? [^borrow w : World e : int] #reads w : bool ```c (void)w;(void)e; return 1; ```)
(defn get! [^borrow w : World e : #refine{ x : int | (alive? w x) }] : int (.n w))
(defn main [] : int (let [^mut w (World 7)] (let [__f (& w)] (println (if (alive? w 0) (get! w 0) -1)))) 0))
EOF
sed 's/module a/module b/' t/a.tur > t/b.tur
for i in $(seq 1 8); do ./build-debug/tur test t >/dev/null 2>&1; echo "run $i: exit=$?"; done
# several runs exit 139 (SIGSEGV); some exit 0
```

## Characterization

- **In-process multi-compile only.** Separate `tur` invocations per file never
  crash. So it is stale process-global state carried across `cmd_build`, not a
  bug in compiling any one file.
- **Nondeterministic** -- consistent with a memory corruption plus ASLR/heap
  layout. A given set of files flips between "usually passes" and "usually
  crashes" across sessions.
- **ASan does not report it** (Debug build has ASan; `abort_on_error=1` still
  yields a bare SIGSEGV with no report). That points at ARENA corruption (the
  bump allocator ASan does not instrument) or a stack overflow, not a
  malloc-heap use-after-free.
- **Worse under `--strict-refine`** (which discharges every obligation, so more
  refine state is built) and with a **recursive** refined function, but it
  occurs without either.

## Partial fix already applied

`cmd_build` did not call `refine_discharge_reset()` (only `check`/`run`/`emit-c`
did), so the global refine memo (`g_memo` in `refine_discharge.c`) kept VC
pointers into the freed per-compile arena; the next compile's `memo_lookup`
dereferenced them through `refine_vc_equal` on a fingerprint collision -- a real
cross-compile use-after-free. Adding `refine_discharge_reset()` at the top of
`cmd_build` (src/main.c) fixes THAT channel and drops the crash rate (8/8 -> 7/8
on one set), but a second corruption channel remains and is the primary cause.

## Why ASan is silent (2026-07-27)

The compiler allocates through a bump `Arena` that ASan does not instrument at
sub-allocation granularity. Two effects hide the corruption:

- `arena_reset` "poisons" reclaimed bytes with a plain `memset(0xDE)` (Debug),
  which reads back as still-valid garbage, not an ASan trap.
- `arena_free` frees slabs via `malloc` free, so compile 2's `arena_init` often
  gets the SAME addresses. A global still holding a compile-1 arena pointer then
  aliases compile-2's VALID data -- ASan cannot flag it, and the eventual crash
  is wherever that aliased memory is later misused, never where the stale
  pointer lives. Hence: nondeterministic, backtrace-useless, ASan-silent.

## Diagnosis status / fix directions

- **One channel fixed:** `cmd_build` did not call `refine_discharge_reset()`; the
  global refine memo (`g_memo`) kept VC pointers into the freed per-compile
  arena. `cmd_build` now resets it (drops the crash rate, 8/8 -> 7/8 on one set).
- **Searched, not yet found:** there is no obvious un-reset global module/import
  cache (`elab_module.c`/`elab_toplevel.c`/`pkg.c` have none), and the only refine
  globals are `g_stats`/`g_memo` (both reset). The remaining channel is
  import-gated (the compiler-repo non-import 2-refined case never crashes) and
  survives the memo reset, so it is a DIFFERENT process-global holding a
  cross-compile arena pointer -- candidate areas: cross-module binding/`Elab`
  resolution reachable from the refine crossing path, or VC/UF interning.
- **The right next step is tooling, not more blind poking.** Build the
  ASan-aware arena poisoning in
  [`docs/archive/arena-debug-poisoning-plan.md`](arena-debug-poisoning-plan.md)
  and run this repro under it: `__asan_poison_memory_region` on reset/free plus a
  no-address-reuse debug mode turns the silent alias into a loud ASan report AT
  the stale deref, whose backtrace names the offending global. Then reset/clear
  it per `cmd_build` like the memo.

Until fixed: do not compile multiple refined files in one process. Run each
refined test as its own `tur run`/`tur check` invocation.

## Consequence

RE1's refined ecs tests cannot be auto-run via `tur test <dir>` until this is
fixed (see `docs/upcoming/v1/ecs-refinement-typed-apis-plan.md` RE1 status).
They are kept under `spices/ecs/tests/refined/` (a subdir `tur test tests` does
not descend into) and are verified individually. The `tur test` per-test-flags /
expect-error directive feature itself is unaffected and general-purpose.
