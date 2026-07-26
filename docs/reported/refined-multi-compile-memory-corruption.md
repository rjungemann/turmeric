# Compiling multiple refined files in one process corrupts memory (nondeterministic SIGSEGV)

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

## Fix directions

Find the remaining process-global refine state that survives `cmd_build` and
holds per-compile-arena pointers (candidates: any static cache in
`refine_collect.c` / `refine_solver.c`, the VC/UF interning, or a global reused
across the elaborator's per-file `Elab`). A reliable repro under a fresh ASan
build with `ASAN_OPTIONS=detect_stack_use_after_return=1` and arena poisoning
would localize it. Until then, do not compile multiple refined files in one
process: run each refined test as its own `tur run`/`tur check` invocation.

## Consequence

RE1's refined ecs tests cannot be auto-run via `tur test <dir>` until this is
fixed (see `docs/upcoming/v1/ecs-refinement-typed-apis-plan.md` RE1 status).
They are kept under `spices/ecs/tests/refined/` (a subdir `tur test tests` does
not descend into) and are verified individually. The `tur test` per-test-flags /
expect-error directive feature itself is unaffected and general-purpose.
