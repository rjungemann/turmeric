# JIT Engine -- Phase J0 spike results

Status: J0 COMPLETE on x86-64 Linux (2026-07-28) and arm64 macOS (2026-07-27,
full corpus 2026-07-28). Sections 0-7 are the original Linux write-up;
**section 8 corrects three of their claims**, and **section 9 corrects three of
section 8's** from the full-corpus macOS run -- read 9 first.
Plan: [docs/upcoming/jit-engine-plan.md](jit-engine-plan.md)

## 0. Verdict

**MIR works. Proceed to J1.** The `reader -> passes -> emit C -> c2mir ->
MIR-gen -> call` pipeline runs real Turmeric programs in process with no `cc`
subprocess and no disk artifacts, and it does so on 89% of a fixture-corpus
sample without any change to the compiler. (**Amended -- see 8.2, 8.4, 8.4.2.**
Both that 89% and the macOS 78% are stride-sampling artifacts of the same
distribution: the full 1,680-fixture corpus on Linux under eager generation is
**84.8%**, and stride-10 subsamples of it span 78.6%-89.3%. Quote 84.8%. All
of these need a subset shim.) (**Amended again -- see 9.1.** Sampling is the
dominant term but not the whole story: the full-corpus macOS run is **81.7%**,
not the ~85% 8.4.2 predicted. 37 of the 51-fixture gap was `__extension__` in
our own codegen, now fixed, which brings macOS to 83.9%.)

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
of the four attributes the emitter uses matter -- and a fifth, `packed`, never
comes from the emitter at all but is the most dangerous of the set (9.3):

| Attribute | Emitted for | Consequence when dropped |
|---|---|---|
| `unused` | ~69 sites per TU | harmless |
| `constructor` | `__tur_cps_register` direct->CPS registry; `pthread_key_create` per dynamic var; `__tur_module_def_init`; `__sk_register` call frames | **SIGSEGV** (effectful indirect call dispatches through a NULL registry entry) or **wrong output** (dynamic var reads its root default) |
| `cleanup(f)` | `_dynvar_pop_*` on scope exit | **wrong output**; no recovery possible outside the compiler |
| `packed` | never by the emitter -- arrives from **system headers and user inline-C** | **wrong struct layout**, silently. See 9.3 |

(**Amended -- see 9.3.** This table lists only attributes the *emitter* emits,
and its "three of the four matter" framing reads as the complete attribute
story. It is not: `__attribute__((packed))` is dropped by the same mechanism,
with a different consequence class -- ABI divergence rather than missing
initialization -- and it reaches every TU through system headers.
`#pragma pack` is dropped too, and is a separate mechanism this section does
not mention at all.)

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
| `__extension__ ({ ... })` | 9 emitter sites | GNU only; c2mir takes `({ ... })` but has no `__extension__` keyword. **This table missed it** because glibc's `sys/cdefs.h` `#define`s the token away when `__GNUC__` is undefined -- which is c2mir's case -- so it is invisible on Linux. Apple's libc has no such fallback. | **FIXED** (`cc5cf8461`): emit the bare form. Was the single largest recoverable class at 37 fixtures. See 9.2. |

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

1. ~~**S1 first, and scope it to three things**~~ **-- DONE, and the
   projection was wrong. See section 11.** All three shipped
   (30c0b7637, e285922de): `__auto_type` 115-225 -> 6-12 per TU, scalar
   `(T){0}` -> 0, literal `__thread` -> 0, suite green at 2399/0. But this
   item predicted "deletes `normalize-c11-subset.py` and takes coverage from
   84.8% to ~96%", and neither followed from the emitter work alone -- the
   measured emitter gain is 84.8% -> 87.7%. The prediction conflated "removes
   almost every occurrence" with "removes the blocking ones"; the normalizer
   was already resolving most sites textually, so S1 moved the needle only on
   the sites it could not type (193 -> 144). Remaining emitter work is
   `emit_fns.c:1738` / `:2083`. Expect a full fixture-snapshot regen.
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
   and is cheap. (**Amended -- see 9.4.** The `__builtin_*` half stands. The
   `atexit` half is **not cheap and not sufficient**: macOS resolves `atexit`
   already, and all three `module-defer-*` fixtures then SIGSEGV at process
   exit instead of failing to link, because the handler is JIT'd code and the
   MIR context is torn down before libc drains its atexit list. Registering
   the symbol on Linux converts a clean diagnosable error into that silent
   crash. J1 must *intercept* `atexit`, not merely resolve it.)
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
  not reached by any sampled fixture. (**Sample artifact -- see 9.5.** At full
  corpus each is reached by exactly one fixture, on both platforms.)
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

> **Superseded -- see 9.1.** This paragraph is the one claim in section 8 that
> the full-corpus macOS run falsifies. Sampling really is the dominant term and
> the offset analysis below stands unchanged, but there *was* a platform
> difference underneath it: macOS full-corpus is 81.7%, not ~85%. The correct
> statement is that sampling explained the 89-vs-78 gap **while masking** a real
> 51-fixture delta, 37 of which were a defect in our own codegen that glibc's
> headers had been concealing.

- **89% was optimistic and should stop being quoted.** The honest Linux eager
  figure is **84.8%**, and section 0 and section 5 are amended accordingly.
  The macOS 78% is equally an artifact; a full-corpus macOS run would be
  expected near 85% too, and is the one measurement still worth taking.
  (**Taken 2026-07-28 -- 81.7%, see 9.1.** The prediction was wrong by 51
  fixtures. The run was worth taking for exactly that reason.)
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

(**Half right -- see 9.1 and 9.3.** The Apple-SDK-header suspicion is confirmed
and lands inside the predicted 4-9 range, at exactly 9 fixtures. But it was not
the residual: it was buried under a 37-fixture class in *generated* code. The
"emit-c output is host-independent" check was sound and is exactly what made
the real cause hard to see -- the emitted text is identical on both platforms;
what differs is that glibc's headers `#define __extension__` away and Apple's
do not. Host-independent output can still fail host-dependently.)

### 8.4.3 What the full corpus found that the sample missed

Running everything surfaced five failure classes no 168-fixture sample
contained, all of them concrete J1 work:

| Class | Count | Reading |
|---|---|---|
| `__auto_type` residue (parse) | 193 | The dominant failure mode, 11.5% of the corpus on its own. Confirms S1 item 1 is the highest-value fix. |
| GNU constructs in user inline-C (parse) | 31 | Step-6 fallback-to-`cc`, by design. |
| `unresolved import: tur_reactor_new` | 10 | S2 boundary -- the harness links 9 runtime TUs and not the reactor. Sizing data for the real symbol table. |
| `unresolved import: atexit` | 3 | **New and load-bearing.** All three are `module-defer-*`. Verified directly: from a `-rdynamic` executable, `dlsym(RTLD_DEFAULT, "atexit")` returns NULL while `printf`, `malloc`, and `abort` all resolve -- glibc ships `atexit` in `libc_nonshared.a`, statically linked into each executable and never exported. J1 must register it explicitly via `MIR_load_external`, exactly as c2m already does for `abort`. This is S4 work, not S2. (**Diagnosis right, fix wrong -- see 9.4.** Resolving the symbol is necessary but not sufficient; macOS resolves it already and the same 3 fixtures SIGSEGV at exit instead.) |
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

## 9. Full-corpus arm64 macOS -- corrections to section 8

Added 2026-07-28 on an Apple M-series (Darwin 27.0.0, AppleClang 21.0.0), MIR
at the same `a8ab7c31` pin, Debug `tur` at `d657707dc`. This is the
full-corpus macOS run 8.4.2 asks for and leaves open.

Full detail, repros and severity analysis:
[docs/reported/jit-macos-full-corpus-extension-and-atexit.md](../reported/jit-macos-full-corpus-extension-and-atexit.md).

### 9.1 The predicted ~85% does not hold -- 81.7%, and sampling was masking a real gap

| Run | Pass | Rate |
|---|---|---|
| Linux, eager (8.4.2) | 1424 / 1680 | 84.8% |
| **macOS, eager, artifacts as of `d657707dc`** | **1373 / 1680** | **81.7%** |
| macOS, eager, after the 9.2 codegen fix | 1409 / 1680 | 83.9% |

8.4.2's sampling analysis is **sound and stands** -- the macOS full run
reproduces the stride effect independently (spread 78.0%-88.7%, 10.7 points),
so stride-on-an-alphabetical-corpus is genuinely the dominant error term, and
recommendation 9 in section 6 needs no change.

What does not stand is "the gap needs no platform explanation and there is no
evidence for one." There was a 51-fixture platform delta; sampling variance was
large enough to hide it. Its composition: **37** the `__extension__` codegen
defect (9.2), **9** Apple SDK headers (9.3), the rest classification drift.

### 9.2 `__extension__` -- a codegen defect glibc was concealing (FIXED)

`elab_sessions.c`, `elab_global.c`, and `emit_expr.c` emitted
`__extension__ ({ ... })` from 9 sites with no platform guard. c2mir accepts
GNU statement expressions but has no `__extension__` keyword.

It never surfaced on Linux because glibc's `<sys/cdefs.h>` `#define`s
`__extension__` to nothing when `__GNUC__` is undefined -- exactly c2mir's
case. Apple's `<sys/cdefs.h>` has no such fallback. So 8.4.4's observation that
the `session-*` fixtures pass under eager on Linux was true only by accident of
glibc's headers, not because the emitted C was in c2mir's subset.

Fixed in `cc5cf8461` by emitting the bare form; the prefix only suppressed a
`-pedantic` diagnostic that the generated-C compile never enables. The three
prefix matchers in `src/turi/eval.c` moved in lockstep -- the interpreter
recognizes these inline-C bodies by text, which is why this could not be a
pure emitter edit. `bash tests/run.sh`: 2399 passed, 0 failed, zero snapshot
churn.

**Method note worth keeping:** 8.4.2 verified that `tur emit-c` output is
host-independent and concluded the residual could not be in generated code.
The verification was correct; the inference was not. Identical emitted text can
still fail host-dependently, because what differs is the libc headers it is
compiled against.

### 9.3 The Apple SDK residue is real (9 fixtures), and c2mir drops struct packing

The 4-9 fixture excess 8.4.2 predicts is confirmed at exactly **9**, and the
suspected cause -- c2mir on Apple SDK headers -- is right. Root cause for 3 of
them generalizes well beyond macOS: **c2mir silently ignores both
`#pragma pack(N)` and `__attribute__((packed))`**.

|  | clang | c2mir |
|---|---|---|
| `#pragma pack(4)` sizeof / offsetof | 20 / 12 | **24 / 16** |
| unpacked control | 24 / 16 | 24 / 16 |
| `__attribute__((packed))` | 12 / 4 | **16 / 8** |

`#pragma pack` at least warns `unknown pragma`; the attribute is silent. This
belongs in 3.1's table, which has been amended.

Severity, stated precisely because the loud case is the harmless one: the 3
affected fixtures are **not miscompiled**. `stdlib/image.tur` includes
`<mach-o/dyld.h>` for `_NSGetExecutablePath(char *, uint32_t *)`; nothing in
`stdlib/` or `src/runtime/` uses a `mach_msg` struct, so XNU's own
`xnu_static_assert_struct_size` turns the layout bug into a clean compile
error. Turmeric uses zero packing itself, so the JIT/host boundary is clean
today. The open vector is user inline-C with a packed struct.

**The platform framing inverts here.** The defect is host-independent. macOS is
not more broken, it is *louder* -- XNU ships `_Static_assert` ABI locks in its
headers and glibc does not, so the same wrong layout on Linux is adopted in
silence.

J1 should **reject rather than mislay**: fail the normalizer on both packing
forms so such programs take the existing step-6 fallback to `cc`.

### 9.4 `atexit` -- right diagnosis, insufficient fix

Section 6 recommendation 8 and 8.4.3 call for registering `atexit` via
`MIR_load_external`. macOS is the natural experiment, because `atexit` **does**
resolve there (verified alongside `printf`/`malloc`/`abort`/`__cxa_atexit`).

It is not enough. All three `module-defer-*` fixtures then print their first
line and **SIGSEGV at process exit** rather than failing to link -- the handler
is JIT'd code, and the MIR context is torn down before libc drains its atexit
list. Registering the symbol on Linux would convert a clean, diagnosable
`unresolved import` into that silent crash.

J1 must **intercept** `atexit`, keep its own deferred-handler list, and drain it
before finalizing the MIR context. That is materially more than one
`MIR_load_external` row, and S4 sizing should reflect it. The `__builtin_*`
half of recommendation 8 is unaffected and still cheap.

### 9.5 Smaller corrections

- **The two omitted atomics are reached.** 8.4 records
  `__atomic_exchange_n` and `__atomic_thread_fence` as "not reached by any
  sampled fixture"; at full corpus each is reached by exactly one, on both
  platforms. Same sample-artifact family as the rest of 8.4.2.
- **The runtime-failure set agrees across platforms.** With 9.2 applied the
  macOS non-parse failures are `dynvar-log-level`, `dynvar-nested`,
  `dynvar-thread-locale`, `self-recursive-carrier-struct-return`,
  `taskgroup-async` (mismatch); `gc-registry-growth`,
  `set-multiword-struct-element` (SIGSEGV); `any-cast-mismatch-panic`
  (SIGABRT, and is supposed to panic -- 8.4.3's caveat applies). Three
  `dynvar-*` mismatches reproduce 8.4.3's finding on a second platform.
- **`Thread local is not implemented`** is warned by c2mir for every
  `_Thread_local` in the TU (10+ per program). 8.4.3 attributes the `dynvar-*`
  mismatches to `__attribute__((cleanup))`; unimplemented TLS is at least as
  plausible and should be ruled out before S-work is scoped on the cleanup
  hypothesis. Platform-independent.
- **`sweep-full.sh` uses `nproc`**, absent on a stock macOS (Homebrew coreutils
  supplied it here). Worth a `command -v nproc || sysctl -n hw.ncpu` fallback.
- **One uncontrolled variable:** this run used a Debug `tur` (contracts live)
  and 8.4.2's build type is unrecorded. The exact agreement on the
  platform-independent classes (`__auto_type` 193/193, inline-C GNU 31/31)
  argues it does not matter; a Release-`tur` macOS run would close it.

### 9.6 Reproducing

```sh
cmake -S . -B build-jit -DCMAKE_BUILD_TYPE=Release -DTUR_JIT_SPIKE=ON
cmake --build build-jit -j --target tur-jit-spike
bash tools/jit-spike/sweep-full.sh              # 1409 / 1680 = 83.9%
```

To recover the pre-fix 81.7% baseline, revert `cc5cf8461` and re-run.

## 10. Linux-side verification of section 9

Added 2026-07-28, x86-64 Linux, gcc 13.3.0, `tur` at `27b4cb399`. Section 9's
claims that are platform-independent were re-run on the other platform; two of
them are strengthened by it, and one of section 9's open questions closes.

### 10.1 Section 9's correction of 8.4.2 is accepted

9.1 is right and 8.4.2 overreached. "The gap needs no platform explanation and
there is no evidence for one" was wrong: sampling was the dominant term, but a
51-fixture platform delta was sitting underneath it.

The specific reasoning error is worth naming, because it is subtle and easy to
repeat. 8.4.2 verified `tur emit-c` output is byte-identical across hosts --
that verification was correct -- and then inferred the residual could not be in
generated code. **Identical emitted text is not host-independent behaviour: it
is compiled against different libc headers.** Confirmed directly here:

```
/usr/include/x86_64-linux-gnu/sys/cdefs.h:493
    #if !(__GNUC_PREREQ (2,8) || defined __clang__)
    # define __extension__          /* Ignore */
```

c2mir does not define `__GNUC__` (probed), so on glibc the token our emitter
produced was erased by glibc's own header before c2mir ever saw it --
`__extension__ ({ 41+1; })` compiles and prints 42 under the spike on this box.
Apple's `<sys/cdefs.h>` has no such fallback. 9.2's account is exact.

### 10.2 The `__extension__` fix verified on Linux

- `bash tests/run.sh`: **2399 passed, 0 failed** at `27b4cb399`.
- Zero snapshot churn confirmed independently: no `tests/fixtures/*/expected.c`
  ever contained `__extension__`, and the four snapshots that do carry `({ ... })`
  statement-expressions still match the current emitter byte for byte.
- The `src/turi/eval.c` half is load-bearing, not incidental: the interpreter
  identifies these inline-C bodies by text prefix, so an emitter-only edit would
  have silently broken session interception in `turi` while leaving the compiled
  path green. Worth remembering that this coupling exists at all.

### 10.3 CLOSED: the `dynvar-*` mismatches are `cleanup`, not TLS

9.5 asks that unimplemented TLS be ruled out before S-work is scoped on the
`__attribute__((cleanup))` hypothesis. Ruled out, on two independent grounds.

**The dynvar machinery does not use TLS.** Its storage is `pthread_key_t` plus
`pthread_getspecific`/`pthread_setspecific` (`_dynvar_key_*`, `_dynvar_root_*`).
Grep for `__thread`/`_Thread_local` anywhere in the dynvar emission: zero hits.
c2mir's unimplemented `_Thread_local` cannot be the mechanism for a feature that
does not use it.

**`cleanup` is confirmed dropped, in isolation.** A `pthread_setspecific`
guarded by a `cleanup` handler, with no Turmeric involved:

| | after scope exit |
|---|---|
| native cc | `getspecific NULL` -- handler ran |
| MIR / c2mir | `getspecific set` -- **handler never ran** |

The two observed dynvar outputs then form a complete causal chain, and each
implicates a different dropped attribute:

- constructors dropped (pre-8.4 state) => `pthread_key_create` never runs =>
  `getspecific` returns NULL => the *root* default is read => `0 0`.
- `cleanup` dropped (current state) => the scope-exit pop never runs => the
  *inner* binding stays installed => `3 3`.

Broken TLS would surface as the root value, i.e. the first shape. We observe
the second. 3.1's attribution stands.

9.5's general caution about `Thread local is not implemented` is still sound and
should not be dropped -- `tur_handler_chain`, `tur_panicking`, and
`tur_current_fiber` *are* `__thread` and do rely on it. It simply is not what
breaks dynamic variables.

### 10.4 The packing defect reproduces on x86-64, wider

9.3 states the packing defect is host-independent and that macOS is merely
louder about it. Confirmed, and the Linux divergence is larger than the arm64
numbers in 9.3:

| | gcc 13.3 (x86-64) | c2mir |
|---|---|---|
| `#pragma pack(4)` sizeof / offsetof | 16 / 4 | **24 / 8** |
| unpacked control | 24 / 8 | 24 / 8 |
| `__attribute__((packed))` | **13 / 1** | **24 / 8** |

A packed struct is nearly twice its true size with every field after the first
at the wrong offset, and on glibc nothing complains -- no `_Static_assert` ABI
lock, no diagnostic for the attribute form. 9.3's "reject rather than mislay"
recommendation is the right call, and this is the strongest argument in the
document for it: the loud platform is the safe one.

### 10.5 Post-fix Linux baseline: unchanged, which is the point

The `__extension__` fix (9.2) was predicted to be inert on Linux, because glibc
had already erased the token (10.1). Confirmed by re-running the full corpus at
`27b4cb399`:

| Run | Pass | Rate |
|---|---|---|
| Linux, eager, pre-fix (`d657707dc`) | 1424 / 1680 | 84.8% |
| **Linux, eager, post-fix (`27b4cb399`)** | **1424 / 1680** | **84.8%** |

Not merely the same total: **zero fixtures changed outcome and zero changed
failure reason**, and the stride spread is bit-identical (78.6%-89.3%). The fix
is a pure macOS gain with no Linux cost or Linux signal, which is exactly what
a header-concealed defect should look like once corrected.

Where that leaves the platform gap:

| | pre-fix | post-fix |
|---|---|---|
| Linux | 84.8% | 84.8% |
| macOS | 81.7% | 83.9% |
| **gap** | **3.1 pts (51 fixtures)** | **0.9 pts (15 fixtures)** |

So 9.2 accounts for roughly three quarters of the real platform delta, and the
residual is 9.3's Apple SDK set (9 fixtures) plus classification drift -- which
is back inside the 4-9 band 8.4.2 originally predicted, arriving there by a
mechanism 8.4.2 had wrongly excluded.

One caveat on the comparison that 9.5 already flags and this run does not fix:
the macOS numbers are from a Debug `tur`, these Linux numbers from the same
Debug build (`./build/tur`, ASan on). That much is at least matched; a Release
run on both would still be worth having before J1 quotes any of it.

### 10.6 `nproc` fallback

Fixed per 9.5: `sweep-full.sh` now takes `nproc`, then `sysctl -n hw.ncpu`,
then 4. Falling back to 1 would make a full sweep ~35 minutes and read as a
hang.

## 11. S1 executed -- measured, and the projection corrected

Added 2026-07-28, x86-64 Linux, same harness and pin. This is
recommendation 1 carried out, and the result does not match what
recommendation 1 predicted.

### 11.1 What shipped

| Construct | Per TU before | After | Where |
|---|---|---|---|
| `__auto_type` | 115-225 | **6-12** | `30c0b7637`, `e285922de` |
| scalar `(T){0}` | 75-139 | **0** | `30c0b7637` |
| literal `__thread` | 11 | **0** | `30c0b7637` |

`bash tests/run.sh`: **2399 passed, 0 failed**, with 140 snapshots
regenerated in the same commits.

`__auto_type` was the one the emitter deliberately used, on the stated
grounds that "the repr heuristic disagrees with the emitted form for some
carrier calls" (`emit_expr.c`). That warning is correct, so nothing is
re-derived: the existing signature side table grew a return-type half,
captured as the literal substring the prototype emitted -- which is by
construction what `__auto_type` deduced. Three recording sites were needed,
because three kinds of callee never reach `emit_fn_forward_decls`:
`extern-c` declarations, ABI spec clones, and ADT ctors (at **two** distinct
emission sites -- `emit_program` emits them inline into `early_file`, and
recording only `emit_adt_typedef_and_ctors` missed the common path).

Two traps in that side table, both real bugs found while wiring it:
`emit_sig_find_or_add` fixes an entry's arity on creation and
`emit_sig_record_param_ctype` refuses to write into an entry whose arity
disagrees, so recording a return type first silently discarded every param
type for that function; and `emit_sig_reset()` lived *inside*
`emit_fn_forward_decls`, which runs after the `early_file` ctor emission, so
anything recorded earlier was wiped before a call site could read it. The
reset now runs once per program at each entry point.

### 11.2 The projection was wrong

Recommendation 1 said S1 "deletes `normalize-c11-subset.py` and should take
coverage from 84.8% to ~96%". Measured:

| Stage | Full corpus | |
|---|---|---|
| Pre-S1 baseline | 1424 / 1680 | 84.8% |
| **S1 emitter work alone** | **1473 / 1680** | **87.7%** |
| + exact normalizer rules (11.3) | 1557 / 1680 | 92.7% |
| + `TUR_APPLY` aggregate-cast fix (`b61cdf578`) | 1559 / 1680 | 92.8% |
| + host-symbol boundary (`7b97d4036`) | **1571 / 1680** | **93.5%** |

The emitter work is worth **+2.9 points**, not +11. The prediction conflated
"removes almost every occurrence" with "removes the blocking ones": the sweep
runs the normalizer, which already resolved most `__auto_type` sites
textually, so S1 could only move the sites it could *not* type -- 193 -> 144.

It also does not delete the normalizer. The residue is entirely indirect
calls (cast function pointers, thunk typedefs, fat-closure member dispatch),
which the emitter still infers **on purpose**. The case that they are all the
int64 carrier is decent, but a decent case is how a silent miscompile lands,
and guarding against exactly that is why `__auto_type` was there.

Remaining emitter work to actually retire the normalizer: `emit_fns.c:1738`
and `:2083`, which emit `__auto_type __ra<id>_<i> = (...)` for call-argument
temps. That site holds the argument `Expr` *and* the callee's populated
param-ctype table, so it can name the type exactly rather than textually.

### 11.3 Exact normalizer rules, and one bad guess

Four rules were added to the spike normalizer to close the measurement gap.
Three are reads off text the emitter itself generated, not inferences:
`INT64_C(n)` -> `int64_t`; `TUR_APPLY<N>_T(R, ...)` -> `R` (the first macro
argument *is* the return type); `(T)(expr)` -> `T`; and `*(T *)(...)` -> `T`.
The cast rule is restricted to recognized primitive spellings or anything
ending in `*`, so `(f)(x)` -- a call through a parenthesized function name --
cannot be misread as a cast.

A fifth was wrong and the sweep caught it. A `.fn(` rule matched the member
call *nested inside a deref-of-cast*,
`*(tur_adt_Option__int *)(intptr_t)(g.fn(...))`, whose value is the struct
rather than the carrier, producing a new failure class ("incompatible types
in assignment to an arithmetic type lvalue") on two fixtures. Anchoring it to
the start of the paren-stripped expression fixed it.

That mistake is the argument for where these rules live. The identical
heuristic in `emit_expr.c` would have been a silent wrong-type miscompile,
and the `cc` path would very likely have accepted it -- `tests/run.sh` would
have stayed green while the JIT diverged. In the normalizer it surfaced as a
diagnostic on the first sweep.

### 11.4 What the 92.7% run still fails

| Class | Count | Reading |
|---|---|---|
| `__auto_type` residue | 58 | Indirect calls; `emit_fns.c` work above. |
| GNU constructs in user inline-C | 31 | Step-6 fallback to `cc`, by design. |
| `unresolved import: tur_reactor_new` | 10 | S2 boundary -- harness links 9 runtime TUs. |
| `unresolved import: __builtin_*` / `atexit` | 10 | Recommendation 8. |
| `initialization of incomplete type variable` | 3 | c2mir checker limitation. |
| ~~`conversion to non-scalar type requested`~~ | ~~2~~ **0** | **FIXED in `b61cdf578`** -- `TUR_APPLY<N>_T` expanded to `(A0)(a)`, a cast to a struct type when `A0` is an aggregate, which is not legal C. gcc accepts it; c2mir does not. The emitter now decides per argument: cast for scalars (load-bearing for the int64 <-> pointer direction), bare for aggregates (where it was provably a no-op). Corpus 1557 -> 1559. Archived at [docs/archive/jit-tur-apply-casts-to-aggregate-param-type.md](../archive/jit-tur-apply-casts-to-aggregate-param-type.md). |
| signals | 2 | Shim's documented atomics hazard (8.4.3). |

Note on that last-but-one row: both fixtures previously failed *earlier*, on
an unresolved `__auto_type`, so fixing S1 is what let c2mir reach the bad
line. A new failure class appearing after a fix is unmasking as often as it
is regression, and the sweep's class tally should be read with that in mind.

### 11.5 Recommendation 8, and what linking half a runtime costs

`7b97d4036` implements recommendation 8: `atexit` **interception** (the JIT
owns the list and drains it before `MIR_gen_finish` unmaps the handlers -- 9.4
established that registering the real `atexit` is not merely insufficient but
actively worse), a `__builtin_*` shim table enumerated from stdlib and the
fixture inputs in one pass, and the `src/async/` TUs linked into the host so
`tur_reactor_new` resolves by address.

Corpus 1559 -> 1571 (93.5%). Every `unresolved import` class is gone.

**But the reactor half traded a clean failure for a crash, and that is the more
useful result.** The ten `reactor-*` fixtures previously failed at link; they
now link and abort (`free(): invalid pointer`). They pass on the `cc` path, so
this is JIT-specific.

The cause is visible in the emitted C's own autolink markers:

```
__tur_autolink__: -lturi
__tur_autolink__: src/runtime/hamt.c -Isrc/runtime
```

`tur build` links **the whole `libturi` archive**. The harness links a curated
list of 16 TUs. A partial runtime is not a smaller version of the whole one --
it is a different one: the emitted preamble carries its own fiber/scheduler
implementation, and resolving *some* of its reactor calls into a host
implementation that expects the host's structures mixes two runtimes that were
never meant to meet. Before this commit the mismatch was invisible because the
symbols simply did not resolve.

**CORRECTION: the diagnosis in the paragraph above was wrong.** It was written
as an inference from the autolink markers and not tested before being recorded.
The harness has since been switched to a `--whole-archive` link of `libturi` --
the same archive `tur build` autolinks -- and **the ten aborts are unchanged**.
A partial runtime is not what breaks them.

What survives, and is worth keeping, is the *boundary discipline* argument,
which is independent of the reactor question: plan section 3.2 step 4 says the
JIT's `MIR_load_external` table *is* the runtime boundary, so that boundary
should be a defined, complete symbol set -- the archive, as `--runtime=lib`
already does on the `cc` path -- rather than a TU list curated by adding
whatever the last sweep failure named. The harness now links the archive for
that reason alone. (It needs one stub: `libturi` is not self-contained --
`src/lsp/lsp.c` calls `tur_collect_symbols`, defined only in `src/main.c` --
which `tur build` never notices because a normal archive link extracts only the
members it needs. Anything wanting the *whole* runtime present, as a JIT does
since it resolves by name at runtime, meets that edge.)

The reactor aborts are now filed as their own open finding with what has
actually been ruled out:
[docs/reported/jit-reactor-fixtures-abort-under-mir.md](../reported/jit-reactor-fixtures-abort-under-mir.md).
The established fact is that the emitted preamble carries its own `static`
ucontext fiber runtime and uses a **JIT-generated function as a `makecontext`
entry point**; the same split exists on the `cc` path, where it works. Settling
it needs a backtrace, not another hypothesis.

The archive link is confirmed **exactly neutral** against the curated list:
1571/1680 both ways, zero fixtures changed outcome. Getting there surfaced one
more real defect: `libturi.a` silently drops `src/runtime/symbols.c` to a
basename collision with `src/compiler/symbols.c` (both become `symbols.c.o`;
the compiler one wins; `tur_sym_register` is absent from the archive).
`tur build` never notices because `libturt_runtime.a` supplies an uncolliding
copy. Filed at
[docs/reported/libturi-symbols-basename-collision.md](../reported/libturi-symbols-basename-collision.md);
the harness compiles the shadowed TU directly until it is fixed.

Stride spread on this run is 91.7%-94.6% (3.0 points) -- narrower than the
10.7 at 84.8%, because variance shrinks as the pass rate approaches 100%.
It is still wide enough that 8.4.2's rule holds: quote the full corpus.

### 11.6 The "__auto_type residue" class was misattributed for the whole document

Every failure table above counts a class labeled "syntax error on identifier"
and reads it as `__auto_type` residue. That attribution is now known to be
substantially wrong, and the correction changes what J1 should conclude from
this document's numbers.

**24 of those fixtures were failing on a normalizer bug, not on emitted C.**
`PROTO_RE`'s separator between return type and function name was optional, so
the ordinary call statement `snprintf(__m, ...)` matched as a *prototype* --
return type `sn`, name `printf` -- and poisoned the table. Every hoisted
`printf` call then emitted `sn __ps_N = (printf(...));`, an opaque parse error.
This was present from the very first sweep, so the 84.8% baseline, the macOS
numbers, and every intermediate figure carry it. The separator is mandatory
now (`346d1e84f`); any identifier that splits two ways was affected.

**Underneath sat one legitimate emitter gap** (`346d1e84f`): extern-c forms on
the `preamble_decls` suppression list (`printf`, `strlen`, `getenv`, `puts`,
...) never had their return types recorded, because the recording added in
`30c0b7637` lived inside the emit branch that suppression skips. Suppression
means "the system header already declares this"; the extern-c form is still the
type authority, and it is now recorded unconditionally.

**Method note, the third of its kind this session:** the misattribution was
built by surveying failure *text* ("what shapes appear in the unresolved
reports"), and it was dismantled by diffing result *sets* fixture-by-fixture
(8.4.2's own technique, which also caught the include-scan gaining zero
fixtures after 51336def8 claimed it would close most of 29 -- one fixture
contributed every `tur_hamt_*` line in the survey). Failure-text surveys have
misled this document twice; result-set diffs have not been wrong yet. J3
tooling should diff, not grep.

The quoted-include scan from `51336def8` stays despite gaining nothing
directly: it is a read of real declarations, and it converted
`hamt-lowering-basic` from a parse failure into a **wrong-output** failure
under MIR (`false`/`2` where `true`/`1` expected) -- a correctness signal on
the HAMT lowering path that a parse error was hiding, now visible for J3.

### 11.7 End state: no unexplained failure classes

With the misattribution unwound (`346d1e84f`) and one final ordering gap
closed (`637fc4c61` -- extern-c return types are now recorded in a pre-pass at
the top of `emit_program`, because the emitter lifts pap-thunk bodies ahead of
the item loop and the per-item record arrived too late for them), the corpus
stands at:

| Stage | Full corpus | |
|---|---|---|
| J0 baseline | 1424 / 1680 | 84.8% |
| S1 emitter (`__auto_type`/zeros/`__thread`) | 1473 | 87.7% |
| exact normalizer rules | 1557 | 92.7% |
| `TUR_APPLY` aggregate-cast fix | 1559 | 92.8% |
| host-symbol boundary | 1571 | 93.5% |
| gs-splitter temps + thunk typedef table | 1603 | 95.4% |
| proto-misparse fix + unconditional extern-c record | 1629 | 97.0% |
| **extern-c pre-pass** | **1631** | **97.1%** |

`tests/run.sh`: 2399 passed / 0 failed at every step.

**The remaining 49 failures contain no unexplained class.** They are exactly:

- **31 x** GNU constructs in *user inline-C* -- the plan's 3.2-step-6
  fallback-to-`cc`, a design decision, working as designed.
- ~~**12 x** signals: 10 `reactor-*` ... and 2 concurrency fixtures~~
  **RESOLVED to 3** (`9a39519f3`, corpus 1640/1680 = 97.6%): the reactor
  aborts were root-caused to a **link-time weak-symbol handshake that cannot
  cross the JIT boundary** (`tur_closure_headers_enabled`;
  [archived report with the proof chain](../archive/jit-reactor-fixtures-abort-under-mir.md)
  -- note the prime suspect named here, `makecontext` entry, was WRONG; the
  fiber machinery is fine under MIR). The harness now syncs weak config
  globals; the six weak `tur_scheduler_*_st` FUNCTIONS carry the same hazard,
  are not value-copyable, and fold into the `__tur_static_init()` J1 work.
  Remaining 3 signals: 2 shim-atomics casualties + `any-cast-mismatch-panic`,
  which panics BY DESIGN (the sweep counts any signal as failure).
- **6 x** wrong output: 3 `dynvar-*` (the `((cleanup))` gap, 3.1),
  `hamt-lowering-basic` + `load-in-imported-module` +
  `self-recursive-carrier-struct-return` (unmasked wrong-answers under MIR,
  each needing its own investigation). **`hamt-lowering-basic` is now
  explained and is NOT a MIR defect**: the P3 `^persistent` lowering compares
  cstr keys by pointer identity and passes the suite only because gcc merges
  identical string literals -- unspecified behavior (C11 6.4.5p7) that c2mir
  does not provide. Reproduced on the plain `cc` path with runtime-built keys,
  no JIT involved. Filed:
  [docs/reported/persistent-map-cstr-keys-identity-compared.md](../reported/persistent-map-cstr-keys-identity-compared.md).
  **`self-recursive-carrier-struct-return` is also explained, in the opposite
  direction: a genuine upstream MIR miscompilation** -- a two-word by-value
  struct return inside an `if/else + goto-backedge` CFG (the emitted tail-loop
  shape) comes back as `{hi, hi}`; 12-line standalone-C repro, present at
  upstream master tip, each boundary condition verified by a one-line change.
  Filed:
  [docs/reported/mir-two-word-struct-return-goto-loop-miscompile.md](../reported/mir-two-word-struct-return-goto-loop-miscompile.md).
  **`load-in-imported-module` closes the set, and lands in a third layer: the
  spike harness itself.** The `(load "stdlib/math.tur")` splice gives the
  program a module-local `static double sqrt(double) { return
  __builtin_sqrt(x); }`; c2mir types the undeclared `__builtin_sqrt` as an
  implicit `int`-returning function, so the call reads the integer return
  register while the harness's shim delivers the value in xmm0 --
  `floor(sqrt(25.0))` came out as 1. The defect was introduced by the
  `7b97d4036` builtin shims: before them these calls failed CLEANLY at link
  (`unresolved import`); supplying addresses without prototypes converted the
  loud failure into silent value corruption -- the precise anti-pattern this
  document keeps cataloguing. Fixed by prototyping the `__builtin_*` family in
  `subset-shim.h`. Corpus 1640 -> 1641 (97.7%), zero regressions.

  The three investigations landed one each in three different layers --
  product (identity-keyed map path), engine (MIR struct-return miscompile),
  and harness (untyped builtin shims) -- which is both the parity sweep doing
  exactly what J3 intends and a caution that the harness is itself a
  component under test.

That composition -- every failure either a recorded decision or an open report
with ruled-out hypotheses -- is the real J0->J1 handoff condition, more than
the percentage. What J1 inherits as *engine* work: the reactor/fiber question,
the `((cleanup))` decision, `__tur_static_init()` (recommendation 2, still
open), S2's prebuilt-preamble latency work, and the three wrong-output
fixtures.
