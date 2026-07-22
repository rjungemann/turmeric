# `tur link` and the build compile/link split

**Status:** Landed (single-file/compile path). Phases 1-4 and 6 are done:
`resolve_autolink_flags`/`link_command_run` factoring; `tur compile`/`tur link`
+ `.link` sidecar; `tur build --split-build`; `tur build --runtime=lib` backed
by the lean non-ASan `libturt_runtime.a`; and the default flipped to `auto`.
Remaining: the ccache CI wiring (Phase 5), and -- as a *separate, larger*
effort -- extending an archive-linked runtime to project/directory builds
(Section 6a explains why that is not a wiring task). Motivated by
`docs/archive/test-suite-runtime-cps-consolidation-and-speed.md` Section 3, which
measured that `ccache` is a **no-op** on the current build path and that the
per-fixture runtime recompile dominates suite wall-clock. This plan adds a
first-class `tur compile` + `tur link` pair, splits `tur build`'s single
compile+link `cc` call into cacheable `-c` compiles plus a link, and finishes
the remaining Section 3 work. The end state is that `tur build` is roughly
`tur compile && tur link` -- the mental model every user already has from
`cc`/`make`.

**Not experiment-gated.** This is a build-system change (a new subcommand + how
`tur build` drives `cc`), which CLAUDE.md's experimental-flags rule explicitly
excludes ("does not apply to ... build-system options"). No `EXPERIMENTS[]` row,
no `--enable=` gate. Behavior is opt-in via flags and defaults preserve current
output byte-for-byte until the split is proven.

## 1. Where we are

`tur build <in> -o <exe>` (and project/`tur run` builds) assemble **one** `cc`
invocation that compiles **and** links in a single call. The assembly is in
`src/main.c` around line 2066:

```c
buf_printf(&cmd, "%s %s -o %s %s", cc, cc_flags, out_path, tmpl);
/* ... then append aux includes/sources and the __tur_autolink__ flags ... */
```

so the effective command is (confirmed by logging `cc` argv on a fixture build):

```
cc <cc_flags> -o <exe> /tmp/tur-build/<name>.c src/runtime/hamt.c <-I... -lturi? -lm ...>
```

Two consequences, both measured in the Section 3 investigation:

1. **`ccache` cannot cache it.** A single `cc` call with multiple `.c` inputs
   and no `-c` is a compile+link; ccache marks it uncacheable. Empirically:
   installing ccache and running 48 fixture builds (stamp cache bypassed)
   produced **48/48 "Uncacheable", 0 hits**. `run.sh:85` already prepends
   `ccache` when present -- it just never gets a cacheable call.
2. **The runtime is recompiled per fixture.** Each build re-`cc`s the autolinked
   runtime sources (`src/runtime/hamt.c`, ...) from scratch. Across ~1442
   build+run fixtures that is the bulk of the ~9-minute suite wall-clock.

A prebuilt runtime already exists but is not used on this path: cmake builds
`libturi.a` (`src/CMakeLists.txt:312`, all runtime TUs), and `main.c:2042-2064`
already drops bare `.c` autolink sources when `-lturi` is present (libturi
supersedes them -- see `docs/archive/tur-eval-import-duplicate-hamt-symbols.md`).
The fixture build path just does not pass `-lturi`; it autolinks bare sources.

## 2. Goal

- Add a `tur compile` subcommand that lowers a `.tur` source to an object file
  (`.tur` -> `.o`, the `emit-c` + `cc -c` steps fused) plus a `.link` sidecar,
  and a `tur link` subcommand that links precompiled objects (+ link flags) into
  an executable / shared library. The documented pipeline becomes
  `tur compile` (cacheable) -> `tur link`, entirely in `tur`'s vocabulary --
  no dropping to raw `cc -c` for the middle step.
- Define `tur build` as `tur compile` + `tur link` composed: it runs the compile
  path per source, then the shared link helper. Splitting its internal `cc` call
  into per-source `-c` compiles + a link lets ccache cache the object compiles
  (runtime objects hit across every fixture and every run) **and** the
  generated-program object hits on unchanged input.
- Keep `emit-c` as-is. It stays the "show me the C" tool and backs the snapshot
  fixtures; `tur compile` is the additive fused `.tur` -> `.o` step, not a
  rename of `emit-c`.
- Ensure `tur link` and the split `tur build` produce **byte-identical binaries**
  to today and compose correctly with every existing knob (`-o`, `--build-dir`
  obj/bin/lib layout, `--shared`, `TUR_CC_FLAGS`, `-lturi`, spice aux sources,
  ASan autodetect at `main.c:2001`).
- Finish the residual Section 3 items (see section 6).

## 3. Design

The pipeline is `tur compile -> tur link`, and `tur build` is those two composed.

### 3a-0. `tur compile`

New canonical subcommand that lowers one `.tur` source to an object file --
the `emit-c` + `cc -c` steps fused, so the workflow never has to drop to raw
`cc`.

```
tur compile [options] <in.tur> -o <out.o>
  <in.tur>          a Turmeric source file
  -o <out.o>        output object (default: <name>.o beside the input / in obj/)
  --cc-flags "..."  extra compile flags (also honors TUR_CC_FLAGS)
  --build-dir <dir> route intermediates through <dir>/{obj,bin,lib}/ (as build)
```

Semantics: run the frontend to lower `<in.tur>` to C (the same lowering `emit-c`
uses -- unchanged, snapshot fixtures still go through `emit-c`), then invoke
`cc <cc_flags> -c <name>.c -o <out.o>`. This is the **cacheable** call: ccache
hits on unchanged input (deterministic obj paths + `CCACHE_NOHASHDIR=1`, already
set at `run.sh:88`).

Because the frontend runs here, `tur compile` already holds the resolved
`__tur_autolink__` / link flags, so it is the natural owner of the **`.link`
sidecar** (3a-A): it writes `<name>.link` next to the `.o`. The `.o` + `.link`
pair is then the complete, link-agnostic unit `tur link` consumes. `emit-c` is
untouched; `tur compile` = `emit-c` lowering + `cc -c` + `.link` emission.

### 3a. `tur link`

New canonical subcommand (register in the `builtins[]` and `CANONICAL_COMMANDS[]`
arrays at `src/main.c:6432` and `:6522`, and in the `main()` strcmp dispatch).

```
tur link [options] <obj-or-source>... -o <out>
  <obj-or-source>   one or more .o objects and/or .c sources
  -o <out>          output executable (or library with --shared)
  --shared          produce a shared library
  --link-flags "..."  extra linker flags (-L, -l, -fsanitize=..., -lm, ...)
  --build-dir <dir> route intermediates through <dir>/{obj,bin,lib}/ (as build)
```

Semantics: `tur link` runs `cc <link-flags> -o <out> <objs/sources...>` with no
`-c`. It reuses the *existing* link-side logic already in `main.c` (the ASan
autodetect at `:2001`, the `-lturi`-supersedes-bare-source filter at `:2042`,
the autolink-path anchoring at `:2013`) -- that code is factored out of the
current monolithic build into a shared `link_objects(...)` helper that both
`tur build` and `tur link` call.

**Autolink-flag flow (the one real design question).** Today the
`__tur_autolink__` link flags are discovered by scanning the *generated C*
(`main.c:1802`). In a split pipeline the linker step must still learn them
without recompiling. Two options, pick in Phase 1:
- **(A) Sidecar file.** `tur compile` writes the resolved autolink/link flags to
  `<name>.link` next to the `.o` (it ran the frontend, so it already has them);
  `tur link` reads it. Explicit, cache-friendly.
- **(B) Re-scan.** `tur link` re-scans the emitted `.c` (or the `.o`'s embedded
  comment is lost, so this needs the `.c`) for `__tur_autolink__`. Simpler, but
  couples link back to the source. **(A) is preferred** -- it keeps `tur link`
  input-agnostic and lets the `.o` + `.link` pair be the complete unit.

### 3b. `tur build` = `tur compile` + `tur link`

`tur build` is defined as the two subcommands composed -- it drives the same
`tur compile` path per source, then the shared `link_objects(...)` helper. There
is no ad-hoc inlined split logic; `tur build` is `compile + link`. This is
default-on once proven, guarded by a `--no-split-build` escape hatch during
rollout that short-circuits back to the monolithic `cc` call unchanged.

1. Compile path (the `tur compile` internals): `emit-c` lowering ->
   `<build-dir>/obj/<name>.c` (+ `.link` sidecar per 3a-A), then
   `cc <cc_flags> -c <src> -o <build-dir>/obj/<hash>.o` for the generated `.c`
   and each autolinked runtime `.c`. These are the cacheable calls -- ccache
   hits on unchanged runtime sources across every fixture and across runs
   (deterministic obj paths + `CCACHE_NOHASHDIR=1`, already set at `run.sh:88`).
2. `link_objects(objs, link_flags, out)` (the `tur link` internals) -> the
   executable.

Because both halves are the real subcommand code paths, `tur build`,
`tur compile`, and `tur link` cannot drift from each other.

The `<build-dir>/{obj,bin,lib}/` layout already documented in CLAUDE.md is the
natural home for the `.o`s; single-file `/tmp/tur-build/` builds get an
`obj/` subdir there.

### 3c. Prebuilt runtime archive (complementary, higher near-term ROI)

Independent of ccache: link the fixture programs against the **prebuilt**
`libturi.a` (or a leaner `libturt_runtime.a` containing just the autolinkable
`src/runtime/*.c` TUs) instead of autolinking bare sources. The
`-lturi`-drops-bare-sources path at `main.c:2042` already makes this safe. A
`tur build --runtime=lib` (and eventually the default) turns "recompile
hamt.c per fixture" into "link a static archive built once per suite run."
Static linking dead-strips to only-referenced TUs, so binary size is unchanged.

This is the cheapest structural win and helps even a cold, ccache-less run.
Order of landing: 3c first (fast, low-risk), then 3a/3b (the general split).

## 4. Interaction with `tur build` -- correctness gates

- **Byte-identical binaries.** A/B a sample of fixtures: `tur build` (old
  monolithic) vs split -- same program output, and ideally same stripped binary.
  The codegen `.c` is unchanged (snapshot fixtures use `emit-c`, untouched), so
  only the `cc` orchestration moves.
- **Full suite green** under the split, run with leak detection on (the
  compiler/codegen path is leak-clean per CLAUDE.md).
- **ASan parity.** The `libturi.a`-ASan autodetect (`main.c:2001`) must still
  fire so a sanitized runtime lib pulls sanitizer flags into the link.
- **`--shared` / spice aux sources / `-I`** paths exercised (project builds,
  `tur run <file>` auto-spice).
- **Escape hatch.** `--no-split-build` restores the monolithic call for a one-off
  if the split misbehaves, until it is proven and the flag is retired.

## 5. Phased plan

1. **[DONE] Factor `link_objects(...)`** out of the monolithic build in `main.c`
   (ASan autodetect, `-lturi` filter, autolink anchoring) with no behavior
   change. Landed as `resolve_autolink_flags(...)` (the four resolution blocks)
   + `link_command_run(...)` (the `cc` assembly/run), plus the shared
   `scan_autolink_markers(...)` and `collect_build_aux(...)` helpers. Full suite
   green (2264 passed, 0 failed).
2. **[DONE] Prebuilt runtime (3c).** `tur build --runtime=lib` (and
   `tur compile --runtime=lib`) links the prebuilt `libturi.a` instead of
   autolinking+recompiling the bare `src/runtime/*.c` sources.
   `apply_runtime_lib_mode()` prepends `-lturi -L<libdir>` to the raw autolink
   when it carries a bare runtime `.c`; the existing `-lturi`-supersedes-bare-.c
   filter in `resolve_autolink_flags` then drops the sources. `libturi.a` is
   auto-located via `$TUR_RUNTIME_LIB` -> `<exe_dir>/src` -> `<root>/build/src`;
   a prefix-installed SDK's lib is picked up by the SDK-anchoring step once
   `-lturi` is on the line. Default remains `--runtime=source` (opt-in for now).
   Output-identical to the source path on the runtime-autolinking fixtures
   (measured: ~15% faster per build on a hamt-only fixture; larger for
   runtime-heavy programs, and it compounds with ccache in step 5).

   **[DONE] Non-ASan runtime archive (the ASan-caveat fix, step-6 prereq).**
   The dev-tree `libturi.a` is the *Debug* build, so it is ASan-instrumented;
   the ASan autodetect in `resolve_autolink_flags` (correctly) pulls
   `-fsanitize=address,undefined` into the link, which turns LeakSanitizer on
   for the *whole program*. Fixtures that intentionally leak process-lifetime
   allocations then fail under LSan, whereas the bare-source build (non-ASan)
   runs them clean.

   Fixed by building a dedicated lean, **non-sanitized** static archive
   `libturt_runtime.a` (CMake target `turt_runtime`, `src/CMakeLists.txt`)
   containing exactly the autolinkable runtime TUs -- `hamt.c`, `symbols.c`,
   `tur_string.c` (the complete set of bare-source `__tur_autolink__` hints).
   `--runtime=lib` now prefers this archive, links `-lturt_runtime`, and drops
   the bare sources itself (`apply_runtime_lib_mode`/`autolink_drop_bare_sources`
   no longer depend on the `-lturi` ASan/filter path). Because the archive is
   non-ASan, the linked program is behaviorally identical to the bare-source
   recompile -- no LSan imposed. It falls back to the full `libturi.a` (and the
   old `-lturi` behavior) when the lean archive is absent.

   Evidence: the **full suite is green under `TUR_RUNTIME=lib`** (2264 passed,
   0 failed) -- the suite builds every compiled fixture through `tur build`, so
   this exercises the runtime-lib link path suite-wide. Plus a direct A/B sweep
   of ~90 runtime-autolinking fixtures (source vs lib) matched byte-for-byte,
   including the previously-LSan-tripping leaky fixtures.
3. **[DONE] `tur compile` + `tur link` subcommands (3a-0, 3a)** + the `.link`
   sidecar (written by `tur compile`). Both commands are registered in
   `builtins[]` / `CANONICAL_COMMANDS[]` and dispatched from `main()`; `emit-c`
   is untouched. The sidecar (`# tur link sidecar v1`) records the fully
   *resolved* link flags (`autolink:` / `asan:` / `cmake:` / `auxsrc:`) so
   `tur link` reproduces the link without re-running the frontend.
4. **[DONE] Redefine `tur build` as `compile + link` (3b)** behind
   `--split-build` (opt-in) / `--no-split-build` (the default remains the
   monolithic single-`cc` call, so output stays byte-identical during rollout).
   A single-file `tur build --split-build` runs the real `cmd_compile` then
   `cmd_link` code paths, so the three cannot drift. Flipping the default and
   wiring ccache measurement is the remaining work here.
5. **[TODO] CI: install + cache ccache**, now that the `cc -c` calls are
   cacheable (was a no-op before -- do NOT do this before the default flips in
   step 4). Cache the ccache dir across runs in `.github/workflows/ci.yml`.
6. **[DONE] Default flipped to `auto`.** The single-file/compile default is now
   `auto` (`g_runtime_mode`): link the lean non-ASan `libturt_runtime.a` when
   locatable, else recompile the bare runtime sources. `auto` never links the
   full (Debug/ASan) `libturi.a` on its own, so a default build is behaviorally
   identical to the old source path -- no link failure when no archive is
   present, no ASan/LSan imposed. `--runtime=lib` forces the archive,
   `--runtime=source` forces recompile, `TUR_RUNTIME=auto|lib|source` seeds the
   default. Full suite green under the flipped default (2264 passed, 0 failed).

## 6a. Project/directory builds -- a *different* runtime architecture

An investigation while wiring runtime-lib established that
`tur build <dir>` / project builds do **not** bare-source-autolink at all, so
`--runtime=lib` does not apply to them and there is nothing to "wire in":

- `cmd_build_multi` and `cmd_build_project` both delegate to
  `cmd_build_multi_files` (`src/main.c`), which emits a **shared runtime owner
  TU** (`tur_runtime.c` + `tur_runtime.h` via `emit_shared_runtime_header`, the
  `emit_runtime_preamble(..., shared=true)` path). The runtime is **embedded
  inline** (owner TU defines the globals; every module TU carries static
  replicas), and the link command appends **no** `__tur_autolink__` flags.
- Confirmed empirically: a project build of a fixture referencing dozens of
  `tur_hamt_*` functions links and runs correctly with no `hamt.c` and no
  `-lturi` on the link line.

So the recompile cost in project builds is the **emitted runtime preamble**, a
codegen concern, not the bare-source autolink the `--runtime=lib` work targets.
Making project builds link a prebuilt runtime instead of embedding it is a
genuine but **separate, larger** optimization: it needs (a) a codegen change to
have `emit_shared_runtime_header` emit *declarations* and link externally
instead of embedding definitions + static replicas, and (b) a **full** non-ASan
runtime archive (the lean 3-TU `libturt_runtime.a` does not cover the whole
preamble; the full `libturi.a` is ASan in Debug, which reintroduces the LSan
problem for project fixture programs). Tracked as a future item, not part of
this plan's compile/link split.

## 6. Finishing the Section 3 work (folded in from the archived report)

The archived report's Section 3 remaining items, tracked to completion here:

- **[this plan, steps 2-5] ccache made effective** -- requires the compile/link
  split; installing ccache alone is a verified no-op. The report's original
  "install ccache = biggest lever" claim was disproved and corrected; the real
  prerequisite is this split.
- **[this plan, step 2] Prebuilt runtime archive** -- the lighter structural win.
- **[DONE] `TUR_TEST_JOBS` clamp** -- an explicit `TUR_TEST_JOBS` above the auto
  cap is now honored (`tests/run.sh`); no longer clamped to 8. Landed already.
- **[note, not this plan] Stamp-cache-in-CI** -- caching `tests/.stamp-cache/`
  helps only incremental runs and is invalidated by any `src/` change (it keys
  on the `tur` binary), so it is low-value for typical PR CI; left as a note.
- **[note] `TUR_WORKER_POOL=1`** -- evaluate defaulting it if stable; orthogonal.

Section 2 of the report (suite splitting / CI `TUR_TEST_SHARD` matrix) is a
separate, independent track (cross-machine parallelism, needs none of this
plan's build changes) and is not covered here.

## 7. Risks / non-goals

- **Risk: link-flag drift between compile and link.** The `.link` sidecar (3a-A)
  is the mitigation -- one source of truth for link flags, emitted by
  `tur compile` alongside the `.o`.
- **Risk: obj cache staleness.** Key object filenames on a content hash of the
  source + `cc_flags` so a flag change re-compiles; let ccache handle the rest.
- **Risk: parallel object compiles inside one `tur build`.** Keep step-2 compiles
  serial within a single build initially; the suite's parallelism is already at
  the fixture level (`xargs -P JOBS`). Intra-build parallelism is a later,
  separate optimization.
- **Non-goal:** changing codegen or the emitted C. This is pure `cc`
  orchestration.
- **Non-goal:** the Section 2 CI-matrix sharding work.
