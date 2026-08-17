# Engine Selection Plan (`:engine` in `build.tur`)

Status: LANDED 2026-08-17 -- P0, E1, E2, and E3 all shipped; every exit
criterion in section 12 is met and pinned by `tests/run-engine-select.sh`
(ctest `tur_engine_select`, runs on every build -- the jit rows assert
whichever outcome the binary has).  Notes against the plan as written:

- P0 took the per-command form the plan offered as the alternative:
  `cmd_jit` locates its input and calls `discover_manifest_reader_macros`
  BEFORE its gates, which both opens the gate for `:experiments [jit]` and
  restores `:reader-macros` for `tur jit`.  The startup hoist was not
  needed and its listed risk (reading manifests for commands that never
  did) is avoided entirely.  Side effect worth knowing: a bare `tur jit`
  now prints usage on EVERY build, so capability probes must key on the
  "carries no JIT" answer to a nonexistent input -- `tests/run-jit.sh`'s
  probe was updated accordingly.
- E1: `:engine` on PkgManifest, validated at parse with the new TUR-E0311
  (unknown VALUES hard-error; unknown KEYS stay silently ignored, which is
  the documented compatibility story).  `resolve_engine` mirrors
  `resolve_build_dir`'s ladder and uses `find_spice_root` (the RM4 walker)
  so explicit-file mode resolves the same manifest project mode does.
- E2: all three unsatisfiability rows behave per the table -- no-JIT-build
  and experiment-off are hard errors (the former with its own message
  naming -DTUR_JIT=ON and the override spellings), and the runtime
  TUR-W0070 cc fallback is untouched.
- E3: the seam is `run_delegate_engine` -- smaller than the sketched
  request struct; the subcommand arms keep their bodies (`cmd_jit` is
  re-entered with a rebuilt argv, the tree-walker via `cmd_eval`).  A
  BARE `tur run` in a Justfile-less build.tur project now falls back to
  the classic project-run path (previously a hard 127), which is what
  makes exit criterion 2's "bare tur run tree-walks the project" reachable
  at all -- a named recipe keeps the task-runner error.  Verbose naming of
  the resolved engine rides TUR_VERBOSE.
- Docs: performance guide (selection row under the engine triangle),
  developing-spices guide (manifest key), and the STALE turi-parity-guide
  claim that turi backs `tur run` is corrected (criterion 7).

The `jit` experiment's graduation remains a separate decision; until then
`:engine "jit"` needs `:experiments [jit]` beside it, which P0 made
sufficient.

Original plan follows.

---

Formerly: PLANNED, not started. Written 2026-08-02. Depends on the `jit`
experiment graduating (currently prototype, expires 0.36.0) for the `jit`
value to be selectable without `--enable=jit`; every other part of this plan
is independent of that graduation.

Let a project state, in its manifest, which execution engine `tur run` should
use — the way it already states `:build-dir` — with a CLI flag and an env var
overriding it in a documented order.

## 0. Scope, and what this plan deliberately is not

**In scope:** a `:engine` key in `build.tur`, a `--engine` CLI flag, a
`TUR_ENGINE` env var, one resolver with the precedence ladder
`resolve_build_dir()` already establishes, and the dispatch seam that lets
`tur run` reach any of the three engines.

**Not in scope, and worth stating loudly:** making the three engines
interchangeable. They are not, they never have been, and this plan does not
pretend otherwise. See section 2 — the language itself has a source-level
construct for the divergence, which is the strongest evidence available that
engine choice is a semantic decision rather than a performance dial.

**Not in scope:** changing which engine `tur repl`, `tur eval`, `tur worker`,
or `tur debug` use. Those are tree-walked by construction today
(`g_interpret_mode = true` at `src/main.c:6989`, `:6960`, `:7296`), and a
JIT-backed REPL *evaluator* is a separate, larger piece of work that no plan
currently covers. J2 made the REPL load spices through the engine; it did not
change how the REPL evaluates expressions.

**Not in scope:** `tur build`, `tur compile`, `tur link`, `tur emit-c`. For
those the C emitter is not a choice, it is the definition of the subcommand.
A `:engine` key must not appear to change them.

## 1. What exists today

Three engines, which are **not three peers**:

| Engine | Source | Availability |
|---|---|---|
| C emitter → `cc` | `src/compiler/emit_*.c`, seam `emit_program()` (`src/compiler/emit_module.c:11362`); driver `compile_to_c()` (`src/main.c:704`) | always built; the reference |
| Tree-walker (turi) | `src/turi/` — `eval.c` (11,949 lines), `env.c`, `value.c`, `interpreter_natives.c` | always built; also `libturi` |
| MIR JIT | `src/jit_engine.c` (703 lines), vendored MIR via `cmake/mir.cmake` | **`-DTUR_JIT=ON` only** (`CMakeLists.txt:104-107`) |

The JIT is **not an independent code generator**. It consumes the same emitted
C and hands it to `c2mir` — `src/main.c:3441-3445`: "Front half is cmd_build's
exactly … The back half hands the buffer to `tur_jit_execute`." So the real
axis structure is:

- **cc vs jit** — one backend, two delivery paths (subprocess `cc` vs
  in-process `c2mir`). A one-way fallback seam between them already exists.
- **compiled vs interpreted** — a genuinely different evaluator.

### Selection is by subcommand, and there is no abstraction

There is no `Backend` enum, vtable, or dispatch struct. (`grep -rni backend
src/` finds only `IOBackend` in `src/async/io.c:12-73`, the "CPS backend"
lowering strategy inside the C emitter, and the LSP analysis backend — none
of them this.) Engines are reached by separate `strcmp` arms in `main()` with
three different signatures:

- `src/main.c:9570` `run` → `cmd_run` (`:3866`) → `cmd_build` (`:2201`) →
  `compile_to_c` + `cc` + `system()`. **C emitter.**
- `src/main.c:9637` `interpret` / `--interpret` → `cmd_eval` → `cmd_eval_h`
  (`:6654`), setting `g_interpret_mode = true` at `:6657`. **Tree-walker.**
- `src/main.c:9564` `jit` → `cmd_jit` (`:3452`), which parses its own argv.
  **JIT.**

Their signatures do not align:

```c
cmd_build (input, out_path, include_dirs, n, target, rm_paths, n_rm)
cmd_eval_h(path, use_color, extra_argv, extra_argc, debug, hooks)
cmd_jit   (argc, argv)
```

The only inter-engine dispatch that exists is the JIT's one-way fallback —
`src/main.c:3589`, `return cmd_run(argc, argv);` under `TUR-W0070` — and it
works only because "cmd_run parses the same argv shape (it never looks at
argv[1])".

### `g_interpret_mode` is a two-valued flag, not an engine selector

Declared `src/runtime/globals.h:143`. It is consumed by the elaborator for the
`#?(:tur … :turi …)` reader conditional at
`src/compiler/elab_toplevel.c:665-679`. The JIT deliberately leaves it `false`
and takes the `:tur` branch (`jit-engine-plan.md` §1.4: "the JIT is a compiled
target"). It cannot be widened to three values without changing what that
reader conditional means, and it must not be.

### The JIT has two gates, and the manifest can open neither

1. **Compile-time** — `-DTUR_JIT=ON`, else `TUR_HAVE_JIT` is undefined and
   `src/main.c:3462-3467` prints "this build carries no JIT engine".
2. **Run-time** — the `jit` experiment, checked at the top of `cmd_jit`:

```c
static int cmd_jit(int argc, char **argv) {
    if (!g_opt_jit) {
        fprintf(stderr, "tur: 'jit' is an experimental feature; enable it "
                        "with --enable=jit\n" ...);
        return 2;
    }
```

**`:experiments [jit]` in `build.tur` cannot open gate 2 today.** The manifest's
experiment list is applied by `apply_manifest_experiments()` (`src/main.c:2501`),
reached only via `discover_manifest_reader_macros()` (`src/main.c:2535`), which
is called by the command wrappers *around* `compile_to_c` — not by
`compile_to_c` itself. `cmd_jit` calls `compile_to_c` directly (`:3494`), never
calls `discover_manifest_reader_macros`, and checks `g_opt_jit` before any
manifest is read. (Side effect: `tur jit` also ignores the manifest's
`:reader-macros`.) Section 4 is about fixing this, and it is a prerequisite.

### `build.tur` parsing, and the precedent to copy

Parsed by `pkg_manifest_read()` (`src/compiler/pkg.c:537`) using the language's
own reader — `read_all` / `detect_lang` / `SourceFile`, with sweet-exp and
`#lang` support. It is **read, not evaluated**: no macro expansion, no
elaboration. Struct: `PkgManifest` (`src/compiler/pkg.h:90-158`). Writer:
`pkg_manifest_write()` (`pkg.c:854`).

The key loop (`pkg.c:630-735`) is a plain `if/else if` chain that **terminates
with no trailing `else`** — verified: the chain's last arm is `:build-opts` and
the loop closes straight into `bool ok = !diag_had_error();` at `:737`.
Unrecognized keys are silently dropped with no diagnostic. This is deliberate
and documented (`docs/guides/developing-spices-guide.md:188`).

`:build-dir` is the existing scalar per-project *setting*, parsed at `pkg.c:667`,
resolved by `resolve_build_dir()` (`src/main.c:1505`) with the ladder documented
at `:1493-1499` and echoed in help at `:7953`:

```
CLI flag > TUR_BUILD_DIR env > build.tur :build-dir > <project-root>/build
```

That function is the shape to copy. `find_spice_root()` (`src/main.c:2453`,
walks up ≤ `TUR_SPICE_WALK_MAX` = 16) is the reusable enclosing-manifest finder.

## 2. Decision: `:engine`, not `:backend`

**Verdict: `:engine`, values `"cc" | "jit" | "interp"`.**

- "backend" is thrice-taken in this codebase (`IOBackend`, the CPS lowering
  backend, the LSP analysis backend) and would be the fourth meaning.
- "engine" is already the word the JIT plan and the performance guide use —
  `jit-engine-plan.md` §0 frames the JIT as "a third execution engine alongside
  the C emitter … and the tree-walking interpreter", and
  `docs/guides/performance-guide.md:704-758` publishes the triangle under that
  name. Reusing it costs nothing and drifts nothing.
- The three values match the performance guide's existing invocation table, so
  the docs already teach the vocabulary.

### This is a semantics key, not a performance key

The engines diverge in what programs *mean*, not just how fast they run:

- **The language has a source-level construct for it.** `#?(:tur … :turi …)`
  exists precisely because authors must write around the compiled/interpreted
  split. A manifest key that flips that split is changing program semantics.
- **Tree-walker carve-outs** (`docs/guides/turi-parity-guide.md`): inline-C is
  `none` — a "permanent carve-out" worked around by ~319 hand-written native
  overrides in `src/turi/interpreter_natives.c` and friends; sessions
  `partial`; maps with inline-C comparators `none`. 47 `requires.compiled`
  fixture markers, plus an automatic skip for any fixture containing a
  ` ```c ` block (`tests/run-turi.sh:110-120`).
- **JIT silent divergence** (`docs/guides/jit-guide.md:396-410`):
  `__attribute__((packed))` is silently ignored by c2mir — "there is no
  diagnostic and no fallback to catch it." Deep recursion can overflow where
  `cc` survives (no sibling-call optimization; `TUR_JIT_STACK_MB` default 64).
  Under lazy generation a generation failure surfaces at first call, past the
  point `TUR-W0070` can catch it (`jit-guide.md:368-372`).

**Consequence for the design:** `:engine` sets a project's *default*, must be
overridable at the CLI, and must fail loudly rather than silently substituting
an engine. See section 6.

## 3. Non-goal: unifying the engines

Do not attempt a `Backend` vtable. The engines differ in their entire
lifecycle — one produces an artifact and `exec`s it, one JITs into the current
process, one never leaves the interpreter's `Expr` walk. What this plan needs
is far smaller: **one normalized "run this entry point with these includes and
these program args" call** that all three can satisfy. Everything else stays
where it is.

## 4. Phase P0 (prerequisite) — move the JIT experiment gate

Standalone, independently shippable, and worth landing on its own merits
because it fixes `:experiments [jit]` generally, not just for this plan.

Today `cmd_jit` checks `g_opt_jit` at `src/main.c:3453` before the manifest is
read. Move manifest experiment application so it happens **before** subcommand
dispatch decides the gate is closed — either by hoisting
`apply_manifest_experiments()` to a startup step that runs once `argv`'s input
path is known, or by having `cmd_jit` call `discover_manifest_reader_macros()`
before its gate check (which also restores `:reader-macros` for `tur jit`).

Prefer the hoist. The per-command placement is why this bug exists, and
`:engine` will want the manifest read at the same early point.

**Exit criteria.** A project whose `build.tur` declares `:experiments [jit]`
can run `tur jit` with no `--enable=` on the command line. `tur jit` honors
manifest `:reader-macros`. No change to `--enable=` precedence
(`XF_SRC_CLI` > `XF_SRC_MANIFEST` > user config).

## 5. Phase E1 — the manifest key and the resolver

1. `char *engine;` on `PkgManifest` (`src/compiler/pkg.h:90-158`).
2. One arm in the key chain next to `:build-dir` (`src/compiler/pkg.c:667`):

```c
} else if (strcmp(kw, "engine") == 0) {
    /* engine-selection-plan: default execution engine for `tur run`. */
    out->engine = form_str_dup(vf);
}
```

3. One line in `pkg_manifest_write()` (`pkg.c:854`) so `tur init` round-trips.
4. `resolve_engine(const char *input_or_root, const char *cli_flag)`, modeled
   directly on `resolve_build_dir()` (`src/main.c:1505`):

```
CLI --engine > TUR_ENGINE env > build.tur :engine > "cc"
```

Document it in the same place and the same shape as the `:build-dir` ladder
(`src/main.c:1493-1499`, help text `:7953`).

**Validation.** Unlike unknown *keys*, an unknown *value* must be a hard error —
`:engine "jitt"` silently running under `cc` is exactly the failure mode this
plan exists to prevent. Allocate a new `TUR-E####` alongside `TUR-E0310`
(unknown experiment) and follow its wording.

**Watch the parser quirk.** `pkg.c:633` — `if (kf->tag != F_KEYWORD) { i -= 1;
continue; }` — the loop advances by 2 and resyncs by 1, so a malformed manifest
can mis-pair keys and values. A new key inherits this; do not add a second one.

## 6. Phase E2 — unsatisfiable engines

Three ways `:engine "jit"` can be unsatisfiable, and they need different answers:

| Condition | Answer |
|---|---|
| Build carries no JIT (`TUR_HAVE_JIT` undefined) | **Hard error.** Name the cause and the fix (`-DTUR_JIT=ON`), as `src/main.c:3462-3467` already does. |
| `jit` experiment not enabled (pre-graduation) | **Hard error**, unchanged from today — but reachable from the manifest once P0 lands. |
| `c2mir` cannot compile this program | **Fall back**, keeping the existing `TUR-W0070` → `cmd_run` behavior. This one is a per-program engine limitation, not a configuration error. |

**Verdict: no silent substitution for the first two.** A project that declares
an engine has declared a semantic requirement; quietly running a different one
is the worst available outcome. The existing `TUR-W0070` fallback stays because
it is a *runtime* capability miss with a warning attached, not a misconfiguration.

## 7. Phase E3 — the dispatch seam

The smallest thing that works: a normalized request struct plus one entry point
that `run` dispatches through.

```c
typedef enum { TUR_ENGINE_CC, TUR_ENGINE_JIT, TUR_ENGINE_INTERP } TurEngine;

typedef struct {
    const char  *input;          /* .tur entry */
    const char **include_dirs;
    int          n_includes;
    char       **prog_argv;      /* after `--` */
    int          prog_argc;
} TurRunRequest;

static int tur_run_with_engine(TurEngine engine, const TurRunRequest *req);
```

`tur run` resolves the engine (E1) and calls this. The three arms keep their
current bodies; `tur_run_with_engine` adapts the request to each signature.
`tur interpret` and `tur jit` stay as explicit subcommands — they are the
override, and removing them would break every existing invocation, script,
and the `tests/run-*.sh` harnesses.

Note the seam is already half-proven: `cmd_jit` → `cmd_run` works because both
accept the same argv shape. The tree-walker is the arm that needs real
adaptation, since `cmd_eval_h` takes a path plus hooks rather than argv.

## 8. Compatibility

Old `tur` binaries **silently ignore** `:engine` (section 1) and run under `cc`.
For a project that merely prefers an engine that is correct. For a project that
*requires* one — anything relying on `#?(:turi …)` branches, or on inline-C that
the tree-walker cannot run — it is a wrong-engine run with no diagnostic.

**Therefore: document `:engine` as requiring a `:tur-version ">=X.Y.Z"`
alongside it** whenever the choice is load-bearing. That key exists for exactly
this (validated at `pkg.c:791`, `TUR-E0621`/`E0622`/`W0623`), and `:entry`
(`src/main.c:4245-4248`) is the standing precedent for a manifest key that older
binaries accept and ignore.

## 9. Testing

- Fixtures for each resolved engine, and for each precedence rung: CLI beats
  env beats manifest beats default.
- Unknown-value diagnostic has a fixture; unknown-*key* silence is asserted so
  the compatibility story is pinned.
- The existing `requires.compiled` (47) / `requires.interp` (5) /
  `requires.cc` (5) markers already express per-fixture engine requirements —
  reuse them rather than inventing a second vocabulary.
- `tests/run-jit.sh:72-78` self-skips when the binary carries no engine; the
  new manifest-resolution tests must skip the same way, or they will fail every
  default build.
- Manifest snippets added to guides are shape-checked by
  `tools/check-guide-pairs.py` via `tur fetch --dry-run`, so a `:engine` example
  in a guide fence gets exercised for free.

## 10. Docs to update

- `docs/guides/package-management-guide.md` — the `build.tur` manifest section.
- `docs/guides/performance-guide.md:704-758` — the engine triangle gains a
  "how to select it" row.
- `docs/guides/jit-guide.md` — the fallback contract now has a manifest entry
  point.
- **`docs/guides/turi-parity-guide.md:12-13` is stale and should be fixed while
  we are here.** It claims turi backs "`tur interpret`, **`tur run`**, `tur
  repl`". `tur run` compiles via `cc` (`src/main.c:3941`). Leaving that
  uncorrected while adding a key that genuinely changes what `tur run` uses
  would be actively misleading.

## 11. Risks

- **The key reads as a performance dial.** Users will set `:engine "jit"` for
  speed and inherit c2mir's silent `packed` divergence. Mitigation is naming
  and docs, not code; the guide entry must lead with the semantic difference,
  not the benchmark.
- **`g_interpret_mode` widening.** The temptation is to make it a three-valued
  engine global. It feeds `#?(:tur/:turi)`; widening it changes the meaning of
  every reader conditional in the corpus. Keep it two-valued and keep the
  engine selection beside it, not inside it.
- **P0 hoisting order.** Applying manifest experiments earlier means reading a
  manifest for subcommands that never did. Anything reached before a manifest
  exists (`tur init`, `tur eval` in a bare directory) must tolerate absence,
  which `find_spice_root()` already does.
- **Engine choice becomes an invisible input to bug reports.** A report saying
  "`tur run` miscompiles" now depends on a manifest three directories up.
  `tur run` should name the resolved engine in `--verbose` output, and any
  crash/diagnostic banner should include it.
- **CI does not exercise the JIT.** `tests/run-jit.sh` self-skips and the ctest
  registrations sit inside `if(TUR_JIT)` (`CMakeLists.txt:611-635`), so
  `:engine "jit"` resolution can regress unnoticed in a default build. At
  minimum, resolution (as opposed to execution) must be tested without a JIT
  build — assert that `cc` is chosen and the error is the right one.

## 12. Exit criteria

1. `:experiments [jit]` in a manifest enables `tur jit` with no CLI flag (P0).
2. `:engine "interp"` in `build.tur` makes a bare `tur run` tree-walk that
   project, and `--engine cc` overrides it.
3. `TUR_ENGINE` sits between them, and `tur run --help` documents the ladder in
   the same words as the `:build-dir` one.
4. `:engine "jit"` on a non-JIT build is a hard error naming `-DTUR_JIT=ON`.
5. `:engine "nonsense"` is a hard error with its own diagnostic code.
6. An old `tur` on a `:engine` manifest still builds and runs, and the guide
   tells authors to pair the key with `:tur-version`.
7. `turi-parity-guide.md`'s `tur run` claim is corrected.
