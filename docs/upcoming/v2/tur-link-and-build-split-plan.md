# `tur link` and the build compile/link split

**Status:** Proposed (not started). Motivated by
`docs/archive/test-suite-runtime-cps-consolidation-and-speed.md` Section 3, which
measured that `ccache` is a **no-op** on the current build path and that the
per-fixture runtime recompile dominates suite wall-clock. This plan adds a
first-class `tur link` step, splits `tur build`'s single compile+link `cc` call
into cacheable `-c` compiles plus a link, and finishes the remaining Section 3
work.

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

- Add a `tur link` subcommand that links precompiled objects (+ link flags) into
  an executable / shared library, so the pipeline can be
  `emit-c` -> `cc -c` (cacheable) -> `tur link`.
- Split `tur build`'s internal `cc` call into per-source `-c` compiles + a link,
  so ccache caches the object compiles (runtime objects hit across every fixture
  and every run) **and** the generated-program object hits on unchanged input.
- Ensure `tur link` and the split `tur build` produce **byte-identical binaries**
  to today and compose correctly with every existing knob (`-o`, `--build-dir`
  obj/bin/lib layout, `--shared`, `TUR_CC_FLAGS`, `-lturi`, spice aux sources,
  ASan autodetect at `main.c:2001`).
- Finish the residual Section 3 items (see section 6).

## 3. Design

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
- **(A) Sidecar file.** `tur emit-c` writes the resolved autolink/link flags to
  `<name>.link` next to the `.c`; `tur link` reads it. Explicit, cache-friendly.
- **(B) Re-scan.** `tur link` re-scans the emitted `.c` (or the `.o`'s embedded
  comment is lost, so this needs the `.c`) for `__tur_autolink__`. Simpler, but
  couples link back to the source. **(A) is preferred** -- it keeps `tur link`
  input-agnostic and lets the `.o` + `.link` pair be the complete unit.

### 3b. `tur build` = compile(-c) + link

`tur build` gains an internal split (default-on once proven; guarded by
`--no-split-build` escape hatch during rollout):

1. `emit-c` -> `<build-dir>/obj/<name>.c` (+ `.link` sidecar per 3a-A).
2. For the generated `.c` and each autolinked runtime `.c`:
   `cc <cc_flags> -c <src> -o <build-dir>/obj/<hash>.o`. These are the cacheable
   calls -- ccache hits on unchanged runtime sources across every fixture and
   across runs (deterministic obj paths + `CCACHE_NOHASHDIR=1`, already set at
   `run.sh:88`).
3. `link_objects(objs, link_flags, out)` -> the executable.

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

1. **Factor `link_objects(...)`** out of the monolithic build in `main.c`
   (ASan autodetect, `-lturi` filter, autolink anchoring) with no behavior
   change. Land + full suite green.
2. **Prebuilt runtime (3c).** `tur build --runtime=lib` links `libturi.a`;
   measure suite wall-clock delta. Make default once green.
3. **`tur link` subcommand (3a)** + `emit-c` `.link` sidecar. Register the
   command; document `emit-c -> cc -c -> tur link`.
4. **Split `tur build` (3b)** behind `--no-split-build`; wire ccache; measure
   cold vs warm suite wall-clock (expect the big drop here). Flip default.
5. **CI: install + cache ccache**, now that calls are cacheable (was a no-op
   before -- do NOT do this before step 4). Cache the ccache dir across runs in
   `.github/workflows/ci.yml`.
6. Retire `--no-split-build` after a green soak.

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
  is the mitigation -- one source of truth for link flags, emitted alongside the
  `.c`.
- **Risk: obj cache staleness.** Key object filenames on a content hash of the
  source + `cc_flags` so a flag change re-compiles; let ccache handle the rest.
- **Risk: parallel object compiles inside one `tur build`.** Keep step-2 compiles
  serial within a single build initially; the suite's parallelism is already at
  the fixture level (`xargs -P JOBS`). Intra-build parallelism is a later,
  separate optimization.
- **Non-goal:** changing codegen or the emitted C. This is pure `cc`
  orchestration.
- **Non-goal:** the Section 2 CI-matrix sharding work.
