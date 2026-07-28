# JIT Engine -- Phase J0 spike results

Status: J0 COMPLETE on x86-64 Linux (2026-07-28) and arm64 macOS (2026-07-27).
Sections 0-7 are the original Linux write-up; **section 8 corrects three of
their claims** -- read it first.
Plan: [docs/upcoming/jit-engine-plan.md](jit-engine-plan.md)

## 0. Verdict

**MIR works. Proceed to J1.** The `reader -> passes -> emit C -> c2mir ->
MIR-gen -> call` pipeline runs real Turmeric programs in process with no `cc`
subprocess and no disk artifacts, and it does so on 89% of a fixture-corpus
sample without any change to the compiler. (**Amended -- see 8.2, 8.4, 8.4.2.**
Both that 89% and the macOS 78% are stride-sampling artifacts of the same
distribution: the full 1,680-fixture corpus on Linux under eager generation is
**84.8%**, and stride-10 subsamples of it span 78.6%-89.3%. Quote 84.8%. All
of these need a subset shim.)

Two things the plan did not anticipate, both actionable:

- The dominant *correctness* hazard is not c2mir's missing C11 features. It is
  that c2mir **accepts GCC attributes and silently discards them**
  (`c2mir.c:4392`). `__attribute__((constructor))` is load-bearing in the
  emitted C, and dropping it produced SIGSEGV in effectful code and wrong
  answers in dynamic variables -- with no diagnostic at all. See section 3.
- The dominant *latency* cost is not the program. It is the ~3,850-line runtime
  preamble that every emitted TU carries, identical program to program: 76% of
  c2mir time and 50% of generation time. S2 (runtime-as-prebuilt-library) is
  therefore not hygiene, it is the whole REPL performance story. See section 4.

Not verified here: **arm64 macOS / MAP_JIT**. This container is x86-64 Linux
only, so the plan's "if the M1 exec path is broken, stop and re-evaluate" gate
is still open. Nothing else in J0 depends on it. (**Closed 2026-07-27 -- see
section 8.1.** MIR handles Apple Silicon correctly and the gate passes. But
8.3 finds the latency case inverts there.)

## 1. What was built

| Path | What it is |
|---|---|
| `cmake/mir.cmake` | MIR vendored via `FetchContent`, pinned to `a8ab7c31cd5f9b23b77d84c60b3d83e62d9d304c` (post-v1.0.0). Inert unless `-DTUR_JIT_SPIKE=ON`. |
| `tools/jit-spike/tur-jit-spike.c` | The harness: C text in, `c2mir_compile` -> `MIR_gen` -> call `main`, with per-phase timing. **The original was never committed (see 8.2); this is a reconstruction.** |
| `tools/jit-spike/normalize-c11-subset.py` | Scaffolding that rewrites emitted C into c2mir's subset. Every rule in it is an S1 item; it is deleted when S1 lands. |
| `tools/jit-spike/subset-shim.h` | Prepended to every TU. Covers three c2mir gaps the normalizer misses (`__thread`, the GCC atomic builtins, `__ATOMIC_*`). A finding, not a fix -- see 8.2. |
| `tools/jit-spike/run-spike.sh` | The J0 exit-criteria set. |
| `tools/jit-spike/sweep-fixtures.sh` | Stride sample, for quick iteration only. **Do not quote its output** -- see 8.4.2. |
| `tools/jit-spike/sweep-full.sh` | Full-corpus sweep, no sampling. The script that produced 84.8%; use this whenever a number will be quoted. |

```sh
cmake -S . -B build-jit -DCMAKE_BUILD_TYPE=Release -DTUR_JIT_SPIKE=ON
cmake --build build-jit -j --target tur-jit-spike
bash tools/jit-spike/run-spike.sh
```

A default `cmake -S . -B build` fetches nothing and builds nothing new --
same posture as the `TUR_REFINE_Z3_ORACLE` block it sits next to.

Symbol resolution follows plan section 3.2 step 4 exactly: the runtime
(`hamt`, `symbols`, `tur_string`, `rc`, `gc`, `rc_free_queue`, `runtime`,
`arena`, `buf`) is compiled *into the harness*, which is linked `-rdynamic`,
and c2mir-emitted references resolve through `dlsym(RTLD_DEFAULT, ...)` by
address. c2mir never sees a line of `hamt.c`.

## 2. Exit criteria

The plan asks for hello + one HAMT fixture + one effects/CPS fixture.
`tests/fixtures/hello` carries an `expected.stdout` with no input file, so
`arith` stands in as the hello-grade case.

| Fixture | Exercises | Result |
|---|---|---|
| `arith` | arithmetic, `println`, comparisons | PASS |
| `hamt-basic` | HAMT via the host-resolved runtime, inline-C `malloc` bodies | PASS |
| `cps-backend-effect` | `defeffect` / `perform` / `handle` / `resume` through `dk_handler` | PASS |

All three match `expected.stdout` byte for byte -- the *compiled* expectation,
per the plan's 1.4 decision that the JIT is a compiled target.

Inline C compiles as ordinary C on this path, exactly as section 1.3 predicted.
The interpreter's ~319 native overrides have no analogue here and none was
needed.

## 3. c2mir's accepted subset, measured

### 3.1 Silently dropped attributes -- the real hazard

c2mir parses `__attribute__((...))` and discards it with no diagnostic. Three
of the four attributes the emitter uses matter:

| Attribute | Emitted for | Consequence when dropped |
|---|---|---|
| `unused` | ~69 sites per TU | harmless |
| `constructor` | `__tur_cps_register` direct->CPS registry; `pthread_key_create` per dynamic var; `__tur_module_def_init`; `__sk_register` call frames | **SIGSEGV** (effectful indirect call dispatches through a NULL registry entry) or **wrong output** (dynamic var reads its root default) |
| `cleanup(f)` | `_dynvar_pop_*` on scope exit | **wrong output**; no recovery possible outside the compiler |

`constructor` is recoverable from outside: the spike collects every constructor
function and calls them at the top of `main`. That turned 3 of 3 SIGSEGVs into
passes. **J1 should not do this by rewriting** -- the emitter should grow an
explicit `__tur_static_init()` called from `main`, which is also the only way
the JIT and the `cc` path can be guaranteed to agree on initialization order.

`cleanup` has no external fix. It is the one correctness gap J0 leaves open,
and it is why `dynvar-nested` still prints `3 3` instead of `3 1`: the inner
binding is now visible but the scope-exit pop never runs. Either the emitter
lowers `cleanup` to an explicit call at each exit edge, or dynamic variables
fall back to `cc` under `tur jit`.

### 3.2 Hard parse errors in *generated* C (S1 work)

| Construct | Sites per TU | Why c2mir refuses | Fix |
|---|---|---|---|
| `__auto_type x = (E);` | 115-225 | GNU only. c2mir registers `typeof` as a keyword (`c2mir.c:5483`) but never wires it into the grammar, so there is no in-language spelling to macro onto. | Emitter must name the type. See below. |
| `(T){0}` for scalar `T` | 75-139 | Rejects scalar compound literals outright ("braces around scalar initializer", `c2mir.c:7781`). C99-legal, so this is a c2mir gap, not ours. | `((T)0)` -- trivial emitter change. |
| `__thread` | ~10 | GNU spelling. `_Thread_local` parses, with a "not implemented" warning; behaves as a plain global, which is correct single-threaded and wrong the moment a fixture spawns. | Emit `_Thread_local`; keep genuinely threaded runtime TUs out of c2mir entirely (S2). |
| `__atomic_*` / `__ATOMIC_*` | few | GCC builtins. | S2: these live only in runtime TUs that should never reach c2mir. |

On `__auto_type`: the spike recovers the type textually for ~92% of sites (the
initializer is a call, and a call's type is its callee's declared return type,
which is in the same TU). The remaining ~8% are not calls -- `INT64_C(0)`,
`(int64_t)(__ps_N)`, bare variables, arithmetic, and calls through a struct
member function pointer (`f.fn(f.env, ...)`) -- and no text rewriter closes
them. `emit_expr.c:2801-2805` explains why `__auto_type` was chosen (the repr
heuristic disagrees with the emitted form for some carrier calls), so this is a
real piece of S1 work, not a search-and-replace.

### 3.3 What c2mir accepted without complaint

Worth recording, because it is most of the surface: `setjmp`/`longjmp`
(56 sites), `ucontext`/`swapcontext` fiber plumbing, function-pointer thunk
typedefs, the whole CPS/DK heap-continuation preamble, designated struct
initializers, `pthread_*`, and every system header the preamble pulls in. The
only warnings on a clean run are "unknown pragma" (3) and "Thread local is not
implemented".

### 3.4 User inline-C

3 fixtures in the sample failed on GNU extensions inside *user* inline-C --
`stdlib/httpd.tur` uses a designated-initializer range (`[0 ... 255] = -1`).
This is exactly the plan's 3.2-step-6 case and needs no fix: fall back to `cc`
with a TUR-W naming the construct. The plan is right not to audit user inline-C.

## 4. Latency

Measured on 4-core x86-64 Linux, gcc 13.3.0, Release-mode harness, best of 5.
The `arith` TU is 7,540 lines / ~292 KB of emitted C containing ~780 functions.

### 4.1 The JIT

| Posture | c2mir | link+gen | execute |
|---|---|---|---|
| eager `-O0` | 92 ms | 70 ms | 0.01 ms |
| eager `-O1` | 89 ms | 90 ms | 0.02 ms |
| eager `-O2` | 89 ms | 125 ms | 0.02 ms |
| eager `-O3` | 99 ms | 148 ms | 0.04 ms |
| **lazy `-O2`** | **91 ms** | **23 ms** | **0.8 ms** |

Lazy generation (`MIR_set_lazy_gen_interface`) is the right default for the
REPL and probably for `tur jit` generally: it moves whole-module generation off
the critical path and only pays for functions actually reached. Total
time-to-first-output drops from ~215 ms to ~115 ms.

### 4.2 Against `cc`

Same TU, same machine:

| Path | Time |
|---|---|
| `cc -O2 -c` (this TU only, no link) | 360 ms |
| `cc -O0 -c` (this TU only, no link) | 480 ms |
| `cc -O2` compile + link with 3 runtime TUs | 1,050 ms |
| `tur build` end to end (subprocess + disk) | 415 ms |
| **JIT, lazy `-O2`, in process** | **115 ms** |

So ~3.6x faster than `tur build`'s current round trip and ~9x faster than a
full `cc -O2` compile+link. Real, but note this is **not** the "~100x faster
than gcc -O2" figure from Makarov's benchmarks -- that number is MIR-gen
consuming MIR IR, and it does not survive contact with the `c2mir` front end,
which is where 44-80% of our time goes. Plan section 0's "sub-millisecond per
function" holds only in the aggregate sense (~0.15 ms/function at 780
functions); no one should expect a sub-millisecond *program*.

### 4.3 Where the time actually goes -- the S2 finding

| Input | Lines | c2mir | gen |
|---|---|---|---|
| trivial `int main` + `<stdio.h>` | 2 | 8 ms | 0.5 ms |
| fixed runtime preamble alone | 3,847 | 68 ms | 68 ms |
| whole `arith` TU | 7,540 | 90 ms | 135 ms |

The first 3,847 lines are **byte-identical** across `arith`, `hamt-basic`, and
`cps-backend-effect`. That fixed preamble is 51% of the lines, 76% of c2mir
time, and 50% of generation time, and it is recompiled from scratch for every
single program.

This reframes S2. The plan lists "runtime-as-library boundary" as optional
pre-work that "shrinks the JIT surface"; it is actually the difference between
a ~115 ms and a ~25 ms compile, and in the REPL -- where the user recompiles on
every edit -- that is the difference between noticeable and invisible. It also
happens to be the same change that keeps atomics and TLS out of c2mir's reach
(3.1, 3.2). **Do S2 before J2.**

## 5. Corpus coverage

`bash tools/jit-spike/sweep-fixtures.sh 200` -- an evenly spaced sample of 168
of the 1,680 fixtures that have `input.tur` + `expected.stdout` and need no
CLI flags, args, or skip markers.

| Outcome | Count |
|---|---|
| PASS (stdout matches compiled expectation) | **150 (89%)** |
| FAIL -- unresolved `__auto_type` (S1, 3.2) | 13 |
| FAIL -- GNU range initializer in user inline-C (step-6 fallback, 3.4) | 3 |
| FAIL -- unresolved runtime symbol `tur_reactor_new` (S2 boundary; the harness links 9 runtime TUs, not the reactor) | 1 |
| output-mismatch -- `dynvar-nested`, dropped `cleanup` (3.1) | 1 |

Zero crashes and zero hangs. This is an *indicative* number, not J3: the sample
excludes flag-driven and stderr-contract fixtures, and it runs against the
normalizer rather than a subset-clean emitter.

**Superseded by 8.4.2 -- do not quote this table.** This sample happens to be
the luckiest of the ten possible stride offsets. The full 1,680-fixture corpus
gives **1,424 (84.8%)**, and the failure mix is materially different from what
168 fixtures showed: 193 `__auto_type` residues rather than 13, and four
unresolved-import classes this sample never touched. 8.4.3 has the real
breakdown.

## 6. Recommendations for J1

1. **S1 first, and scope it to three things**: stop emitting `__auto_type`,
   emit `((T)0)` instead of `(T){0}`, emit `_Thread_local` instead of
   `__thread`. `__auto_type` alone is 193 of the 256 full-corpus failures
   (8.4.3) -- 11.5% of the whole fixture set -- so this single fix is worth
   more than everything else in this list combined. That deletes
   `normalize-c11-subset.py` and should take coverage from 84.8% to ~96%.
   Expect a full fixture-snapshot regen in the same PR.
2. **Emit an explicit `__tur_static_init()`** called from `main` rather than
   relying on `__attribute__((constructor))`. This is a correctness fix for the
   JIT and a legibility win for the `cc` path.
3. **Decide `__attribute__((cleanup))`.** Either lower it explicitly at exit
   edges, or make dynamic variables a documented `cc`-only feature under
   `tur jit` (step-6 fallback with a TUR-W).
4. **Promote S2 ahead of J2**, per section 4.3.
5. ~~**Default to lazy generation** (`MIR_set_lazy_gen_interface`).~~
   **WITHDRAWN -- see 8.1 and 8.4.** Lazy generation has two independent
   defects at this pin: it is not re-entrant (8.1), and it miscompiles pthread
   entry functions even single-threaded (8.4.1). Full-corpus cost is 17
   fixtures, 16 of them session-types (8.4.4). Generate eagerly, and revisit
   only with a lock and a fix for the codegen bug.
6. ~~**Verify arm64 macOS MAP_JIT** before the `EXPERIMENTS[]` row lands.~~
   **Done, 2026-07-27 -- gate closed, see 8.1.** Replaced by three new items:
   (a) move the atomic builtins into the host runtime instead of emitting them
   as text, and emit `_Thread_local` rather than `__thread` (8.2); (b) make
   generation safe under concurrent first-call before defaulting to lazy, per
   recommendation 5 (8.1); (c) re-scope S2 into J1 and re-measure -- on Apple
   Silicon the JIT is at parity with `cc` without it (8.3).
7. The plan's step-6 fallback-to-`cc` is confirmed necessary and sufficient for
   user inline-C. Do not add the `:jit` reader-conditional key from 1.4 -- the
   31 full-corpus failures in this class are a handful of stdlib constructs,
   not a pattern needing new syntax.
8. **Register `atexit` and the `__builtin_*` family via `MIR_load_external`**
   (8.4.3). `atexit` is not in the dynamic symbol table, so `dlsym` cannot
   reach it and every `module-defer-*` program fails to link. This is S4 work
   and is cheap.
9. **J3 must run the whole corpus, or shuffle with a seed -- never stride.**
   `tests/fixtures/` is alphabetical, so stride sampling draws correlated
   clusters and its output swings 10.7 points by offset alone (8.4.2). That
   variance is what produced both the 89% and the 78% in this document. A full
   Linux run is ~9 minutes on 4 cores; there is no reason to sample at all.

## 7. Risks, revisited

The plan flagged three MIR risks up front. After J0:

- **Single maintainer / slow cadence** -- unchanged, and unmitigated by
  anything J0 did. The pin is a commit, so we are never surprised.
- **C11 minus atomics/VLAs/complex** -- accurate but incomplete. The costly
  gaps were `__auto_type`, scalar compound literals, and silently discarded
  attributes; no VLA or `_Complex` use was found in generated output at all.
  (**Wrong -- see 8.2.** The atomics half of this risk was real and was missed:
  every emitted program uses ~17 GCC atomic builtins, which c2mir does not
  support at all. `__thread` is a second unsupported construct in every
  program.)
- **Apple Silicon MAP_JIT** -- **verified 2026-07-27; gate closed** (8.1). MIR
  needed no changes. The residual Apple Silicon concern is not W^X but that the
  latency argument for the whole feature does not hold there without S2 (8.3).

One risk the plan did not list, now the top one: **c2mir fails silently on
constructs it merely ignores.** A parse error is cheap -- it names a line. A
dropped `constructor` attribute cost a SIGSEGV with no diagnostic, and a
dropped `cleanup` produces a plausible wrong answer. J3's parity sweep is
therefore not optional polish; it is the only mechanism that would catch the
next attribute we start emitting.

## 8. arm64 macOS (Apple Silicon) -- gate CLOSED, with corrections

Added 2026-07-27 on an Apple M2 (macOS 27.0.0, Apple clang), MIR at the same
`a8ab7c31` pin. This section closes the one gate J0 left open, and corrects
three things sections 0-7 got wrong. **Read 8.2 before trusting any number in
sections 2-5.**

### 8.1 The MAP_JIT gate is closed -- MIR is fine on Apple Silicon

MIR's `mir-code-alloc-default.c` already does the whole Apple Silicon dance
under `defined(__APPLE__) && defined(__aarch64__)`: `mmap(..., MAP_JIT)`,
`pthread_jit_write_protect_np()` around writes, and `sys_icache_invalidate()`
before execution. Nothing had to be added.

All three J0 exit-criteria fixtures (`arith`, `hamt-basic`,
`cps-backend-effect`) pass byte-for-byte against `expected.stdout`. A
166-fixture evenly spaced sample: **129 pass (78%)**, 34 parse failures, 2
wrong output, 1 abort. No failure in the sweep was attributable to code
allocation, W^X, or instruction-cache coherency.

W^X was probed directly, since per-thread write protection is *the* Apple
Silicon JIT hazard:

| Probe | Result |
|---|---|
| Codegen triggered on a **non-main thread** | pass -- per-thread W^X handled |
| **Eager** gen, JIT'd code then run from 4 threads | pass |
| **Lazy** gen, 4 threads racing the same first call | **MIR assertion** in `_MIR_duplicate_func_insns` (`mir.c:2749`) |

The third row is **not** a MAP_JIT bug and is not arm64-specific: MIR's lazy
stub generation is not re-entrant, so two threads entering the same
not-yet-generated function race. It matters for us specifically because
recommendation 5 in section 6 is "default to lazy generation" and Turmeric has
`spawn`, fibers, and a work-stealing scheduler. **J1 must either serialize
generation behind a lock or generate eagerly for any program that can spawn.**

Unrelated but worth deleting: `cmake/mir.cmake` defines `MIR_PARALLEL_GEN`, but
at pin `a8ab7c31` that macro appears only in MIR's own `CMakeLists.txt` and is
read by no source file. It is a no-op.

### 8.2 CORRECTION: the harness was never committed, and the subset gaps are worse than reported

`tools/jit-spike/tur-jit-spike.c` -- the file section 1 lists as "the harness"
-- **was never tracked by git.** `.gitignore` carries a blanket `*.c` / `*.h`
with negations for `src/`, `tests/`, `examples/`, and `docs/`, but none for
`tools/`, so `git add` skipped it silently and the J0 commit shipped a
`CMakeLists.txt` whose only `add_executable` source did not exist. The branch
could not be built by anyone. Fixed here by adding `!tools/**/*.c` and
`!tools/**/*.h` and committing a **reconstruction** of the harness.

That matters beyond the inconvenience, because the reconstruction does not
reproduce section 5's results, and the reason is not macOS. c2mir rejects three
constructs that Turmeric's *fixed preamble* emits unconditionally -- they are
present in `arith`, the most trivial fixture in the corpus -- and none of them
is guarded by a platform `#if` in `src/compiler/emit_module.c`:

| Construct | c2mir support | Emitted |
|---|---|---|
| `__thread` | none -- only `_Thread_local` is registered (`c2mir.c:5453`); no `kw_add` for the GNU spelling on any target | ~9 per program, literal strings in `emit_module.c` |
| `__atomic_load_n` / `store_n` / `add_fetch` / `compare_exchange_n`, `__ATOMIC_*` | **zero occurrences anywhere in the MIR tree** | ~17 per program |
| `__auto_type` | none | normalizer rewrites most; the residue is 25 of the 34 sweep parse failures |

`normalize-c11-subset.py` handles none of the first two. Section 7 says the
"C11 minus atomics" risk was "accurate but incomplete" and that the costly gaps
were elsewhere; that is wrong -- atomics are used by every single program.
Section 0's "89% ... without any change to the compiler" cannot be reproduced
from the committed artifacts, and the lost harness must have been doing
something equivalent to `tools/jit-spike/subset-shim.h` (added here) via
`c2mir_options.macro_commands`. **Treat 89% as unverified.** The 78% measured
here is with the shim applied and is the first reproducible number in this
document.

The shim is deliberately a readable file rather than a pile of `-D` flags, and
it is not a fix. Its atomic lowerings drop atomicity outright; they are sound
only because the fixtures are single-threaded, and would silently corrupt the
refcount under `spawn`. The real fix is that these belong in the **host
runtime**, compiled by `cc` and resolved by address through
`dlsym(RTLD_DEFAULT)` exactly as `hamt.c` already is -- they should never reach
c2mir as text at all. `__thread` should simply be emitted as `_Thread_local`,
which every supported `cc` also accepts.

### 8.3 CORRECTION: the latency case inverts on Apple Silicon

Best of 5, M2, on an ~8,000-line TU, against section 4's Linux figures:

| | Linux (section 4) | macOS M2 |
|---|---|---|
| JIT: c2mir + link/gen | 91 + 23 = **114 ms** | 178 + 20 = **198 ms** |
| `tur build` | 415 ms | **~190 ms** |
| `cc -O2` compile+link | 1,050 ms | **~220 ms** |

**The JIT is at parity with -- or slightly slower than -- simply shelling out to
`cc` on this machine.** The Linux win came from a slow 4-core container running
gcc 13, not from MIR being fast in absolute terms; an M2 with Apple clang does
the whole `cc` path in ~200 ms. Section 0's framing of the JIT as the "REPL
performance story" does not survive this.

Section 4's preamble finding is not just confirmed but understated. `arith`
(7,560 lines) takes 169.9 ms in c2mir and `hamt-basic` (8,076 lines) takes
178.1 ms -- essentially flat, so nearly all c2mir time is fixed preamble cost
regardless of program size. Of that, ~36 ms is the macOS SDK headers alone
(measured with an 8-line `stdio.h` program), which are heavier than glibc's.

The conclusion for J1/J2 is sharper than section 6.4's "promote S2 ahead of
J2": **on Apple Silicon, S2 is not a prerequisite for good latency, it is the
entire justification for the feature.** Without a prebuilt preamble there is no
measurable reason to ship a JIT on macOS at all. S2 should be re-scoped as J1
work and its projected win re-measured on both platforms before the
`EXPERIMENTS[]` row is written.

### 8.4 Re-measured on Linux with the committed reconstruction

Added 2026-07-28, same x86-64 Linux container as sections 0-7, running the
artifacts as committed in `2bb8c8b6` (reconstruction + `subset-shim.h`), same
168-fixture sample:

| Harness | Generation | Sample result |
|---|---|---|
| reconstruction (`2bb8c8b6`) | eager (`--eager`) | **150 / 168 (89%)** |
| reconstruction (`2bb8c8b6`) | lazy (its default) | 148 / 168 (88%) |
| original (recovered from the working tree) | eager (its default) | 150 / 168 (89%) |

The eager row reproduces section 5 exactly -- same count, same failure
breakdown (13 unresolved `__auto_type`, 3 GNU range initializers in user
inline-C, 1 unresolved `tur_reactor_new`, 1 `dynvar-nested` mismatch). So
**8.2's "treat 89% as unverified" was correct about the commit and is now
stale about the artifacts**: with the harness actually tracked, the number
reproduces from a clean checkout. What was unverifiable was never the
measurement, it was the missing file -- and that is on the original commit.

Two refinements to 8.2 while the record is being set straight:

- The reconstruction is behaviourally equivalent to the original on this
  sample. The two atomics the shim omits relative to the original prologue
  (`__atomic_exchange_n`, `__atomic_thread_fence`/`__sync_synchronize`) are
  not reached by any sampled fixture.
- Section 3.2's table did list `__thread` and `__atomic_*`/`__ATOMIC_*` as
  subset gaps with fixes. What was wrong was section 7's *summary*, which
  said the costly gaps were elsewhere; the shim header's reading of that as
  "the atomics half was missed" overstates it, but the correction stands --
  ~17 atomic builtins per program is not a footnote, and section 7 filed it
  as one.

The 89% -> 78% gap is therefore **not** explained by the reconstruction or by
the shim. Section 8.4.2 runs the A/B and closes it.

### 8.4.1 A second lazy-generation defect -- single-threaded

Isolated while chasing the delta above, and distinct from 8.1's re-entrancy
race: lazy generation miscompiles pthread entry functions with **no
concurrency involved at all**.

```
$ tur-jit-spike -O 2 session-project-basic.subset.c            # lazy (default)
undeclared reg 21 of func tur_session_thread_wrapper
$ tur-jit-spike -O 2 --eager session-project-basic.subset.c    # eager
42
```

Reproduces on `session-project-basic` and `defstruct-field-session-project`,
on Linux, at both `-O0` and `-O2`, on both harnesses. `undeclared reg N of
func` is a MIR-gen internal error, not a c2mir parse failure, so this is a bug
at the pin rather than a subset gap of ours.

Taken with 8.1, lazy generation now has two independent defects and no longer
has a defensible default. Section 6 recommendation 5 is withdrawn. Note the
cost: eager generation is what section 4.1 measured at 125 ms of link+gen
against lazy's 23 ms, so the honest Linux JIT figure is ~215 ms, not ~115 ms
-- which narrows the Linux win over `tur build` (415 ms) from ~3.6x to ~1.9x
and moves it toward 8.3's macOS finding rather than away from it.

### 8.4.2 A/B: both published numbers are sampling artifacts

The 89%-vs-78% gap needed a direct A/B, so the sampling variable was removed
outright: **every eligible fixture, not a stride sample.** Linux, eager, same
shim and normalizer, 1,680 fixtures.

**Full corpus, eager: 1,424 / 1,680 = 84.8%.** (Lazy: 83.8% -- see 8.4.4.)

Then the same stride-10 scheme `sweep-fixtures.sh` uses was replayed against
those full results, once per starting offset:

| offset | pass | parse | wrong output | abort | crash | unresolved |
|---|---|---|---|---|---|---|
| 0 | 143 (85.1%) | 23 | 0 | 0 | 0 | 2 |
| 1 | 146 (86.9%) | 21 | 0 | 0 | 0 | 1 |
| 2 | 145 (86.3%) | 21 | 1 | 0 | 0 | 1 |
| 3 | 141 (83.9%) | 25 | 0 | 0 | 1 | 1 |
| 4 | 143 (85.1%) | 24 | 0 | 0 | 0 | 1 |
| **5** | **132 (78.6%)** | 30 | 0 | 0 | 1 | 5 |
| 6 | 143 (85.1%) | 20 | 1 | 0 | 0 | 4 |
| 7 | 138 (82.1%) | 25 | 1 | 1 | 0 | 3 |
| 8 | 143 (85.1%) | 22 | 0 | 0 | 0 | 3 |
| **9** | **150 (89.3%)** | 16 | 1 | 0 | 0 | 1 |
| **full** | **1424 (84.8%)** | 227 | 4 | 1 | 2 | 22 |

**The spread is 78.6% to 89.3% -- 10.7 points -- on one platform, one
harness, one binary, one generation mode.** The original Linux sample landed
on offset 9, the single luckiest of the ten. The macOS 78% sits at offset 5,
essentially the unluckiest. The reported category profile matches too: macOS
saw 34 parse failures / 2 wrong output / 1 abort out of 166; offsets 5 and 7
give 30/0/1 and 25/1/1 out of 168.

So **the gap needs no platform explanation and there is no evidence for one.**
Both numbers are the same ~85% distribution sampled at different offsets. Two
consequences:

- **89% was optimistic and should stop being quoted.** The honest Linux eager
  figure is **84.8%**, and section 0 and section 5 are amended accordingly.
  The macOS 78% is equally an artifact; a full-corpus macOS run would be
  expected near 85% too, and is the one measurement still worth taking.
- **The sampling scheme itself is the defect, and J3 must not inherit it.**
  `tests/fixtures/` is alphabetical, so consecutive entries are near-duplicates
  by construction -- every `httpd-*` adjacent, every `dynvar-*` adjacent, every
  `cps-backend-*` adjacent. Stride sampling therefore draws strongly correlated
  clusters, and the effective sample size is far below 168. J3 should run the
  whole corpus (this took ~9 minutes on 4 cores) or use a seeded shuffle;
  `sweep-fixtures.sh` keeps the stride only for quick iteration and its output
  should not be quoted as a coverage figure.

One residual, stated as the open question it is: macOS's 34 parse failures
exceeds every Linux offset (max 30). If that survives a full-corpus macOS run
it is a real but second-order excess of roughly 4-9 fixtures, and the natural
suspect is c2mir on Apple SDK headers rather than anything in generated code --
`tur emit-c` output is host-independent (verified: zero `__APPLE__`/`__MACH__`
in the emitted text, the only platform split is `_WIN32`, and no host-
conditional emission exists in `src/compiler/emit_*`).

### 8.4.3 What the full corpus found that the sample missed

Running everything surfaced five failure classes no 168-fixture sample
contained, all of them concrete J1 work:

| Class | Count | Reading |
|---|---|---|
| `__auto_type` residue (parse) | 193 | The dominant failure mode, 11.5% of the corpus on its own. Confirms S1 item 1 is the highest-value fix. |
| GNU constructs in user inline-C (parse) | 31 | Step-6 fallback-to-`cc`, by design. |
| `unresolved import: tur_reactor_new` | 10 | S2 boundary -- the harness links 9 runtime TUs and not the reactor. Sizing data for the real symbol table. |
| `unresolved import: atexit` | 3 | **New and load-bearing.** All three are `module-defer-*`. Verified directly: from a `-rdynamic` executable, `dlsym(RTLD_DEFAULT, "atexit")` returns NULL while `printf`, `malloc`, and `abort` all resolve -- glibc ships `atexit` in `libc_nonshared.a`, statically linked into each executable and never exported. J1 must register it explicitly via `MIR_load_external`, exactly as c2m already does for `abort`. This is S4 work, not S2. |
| `unresolved import: __builtin_*` | 7 | `pow` x4, `strlen`, `popcount`, `memcpy`. All from **inline C**, not from generated code: `stdlib/math.tur:93` calls `__builtin_pow` (which is why 4 unrelated fixtures trip it), and three fixtures use `__builtin_strlen`/`popcount`/`memcpy` directly. c2mir implements no GCC builtins, so each is emitted as an ordinary external call and then fails to resolve. Cheapest fix is a small `MIR_load_external` table mapping the common builtins to their libc equivalents; `stdlib/math.tur` should arguably just call `pow` instead. |
| `initialization of incomplete type variable` | 3 | c2mir checker limitation, all on fat-closure readback fixtures. |

Two crashes (`gc-registry-growth`, `stm-stress`) are the shim's own documented
hazard rather than a MIR defect: both are concurrency fixtures, and the shim
lowers atomics to plain memory ops. That is the predicted corruption, observed.

The four wrong-output fixtures are `dynvar-log-level`, `dynvar-nested`,
`dynvar-thread-locale`, and `self-recursive-carrier-struct-return`. Three of
four being `dynvar-*` confirms 3.1's `__attribute__((cleanup))` finding
generalizes to the whole dynamic-variable feature rather than being one
fixture's quirk.

One classification caveat: `any-cast-mismatch-panic` is counted as an abort,
but the fixture is *supposed* to panic. The sweep treats any signal as a
failure, so the true pass count is marginally higher than 1,424.

### 8.4.4 Eager vs lazy, full corpus

The other axis on which the two published runs differed: my original harness
defaulted to eager, the reconstruction defaults to lazy, and
`sweep-fixtures.sh` never passes `--eager` -- so **the macOS 78% was measured
lazily and the Linux 89% eagerly.** Both modes were run over the full corpus:

| Mode | Pass | Rate |
|---|---|---|
| eager (`MIR_set_gen_interface`) | 1,424 / 1,680 | **84.8%** |
| lazy (`MIR_set_lazy_gen_interface`) | 1,407 / 1,680 | **83.8%** |

17 fixtures regress under lazy; **zero improve**. The regressions are not
scattered -- 16 of 17 are the session-types feature:

```
session-calc-rpc  session-choose-left  session-choose-right  session-effects
session-mp-calc  session-mp-delegated  session-mp-effects  session-mp-handshake
session-mp-ping  session-mp-three-role  session-project-basic
session-project-choice  session-send  session-stm  session-timeout-ok
defstruct-field-session-role        (+ gc-heap-struct-rc)
```

with three distinct symptoms: `undeclared reg N of func
tur_session_thread_wrapper` (11), SIGSEGV (4), and `undeclared func reg
U0_fat@1` / `i_25` (2). Every one of them spawns a pthread. That is the same
root as 8.1's Apple Silicon assertion and as 8.4.1's single-threaded repro,
now with the blast radius measured: **lazy generation is unusable for any
program that spawns.**

So the mode difference contributes ~1 point corpus-wide -- real, but an order
of magnitude smaller than the 10.7 points sampling contributes. It does not
explain the macOS gap either; sampling already accounts for all of it.

### 8.5 Reproducing

```sh
cmake -S . -B build-jit -DCMAKE_BUILD_TYPE=Release -DTUR_JIT_SPIKE=ON
cmake --build build-jit -j --target tur-jit-spike
bash tools/jit-spike/run-spike.sh            # 3 passed, 0 failed on M2
bash tools/jit-spike/sweep-fixtures.sh       # indicative corpus sample
```

`run-spike.sh` and `sweep-fixtures.sh` now pass `--shim
tools/jit-spike/subset-shim.h`; override with `SHIM=` to measure the raw
unshimmed subset gap (every fixture fails).

For any figure that will be quoted, use the full sweep instead of the stride
sample (8.4.2), which also prints the stride spread so the drift stays visible:

```sh
bash tools/jit-spike/sweep-full.sh              # eager; ~9 min on 4 cores
GENMODE= bash tools/jit-spike/sweep-full.sh     # lazy
```
