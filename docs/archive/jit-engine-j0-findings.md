# JIT Engine -- Phase J0 spike results

Status: J0 COMPLETE on x86-64 Linux (2026-07-28) and arm64 macOS (2026-07-27,
full corpus 2026-07-28); **S1 landed 2026-07-28 (section 11) and S1b
2026-07-29 (section 12)**, which is the pre-work J1 depends on. Sections 0-7
are the original Linux write-up; **section 8 corrects three of their claims**,
and **section 9 corrects three of section 8's** from the full-corpus macOS run
-- read 9 first. Current Linux full-corpus coverage: **1647/1680 (98.0%)**,
every remaining failure a recorded decision or a filed report (12.6).
Plan: [docs/archive/jit-engine-plan.md](jit-engine-plan.md)

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
  (**Closed 2026-07-29 -- see section 12.** Both `constructor` and `cleanup`
  are now recovered by the emitter rather than relied upon: an explicit
  `__tur_static_init()` and an explicit scope-exit pop. What remains of this
  hazard is `packed`/`#pragma pack`, which never came from the emitter.)
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
2. ~~**Emit an explicit `__tur_static_init()`** called from `main` rather than
   relying on `__attribute__((constructor))`.~~ **DONE (`77a4f1209`), and
   unlike recommendation 1 it made no prediction to get wrong. See section
   12.** Seven emission sites, corpus unchanged at 1642/1680 -- the value is
   that normalizer rule 3 is retired, not that the number moved.
3. ~~**Decide `__attribute__((cleanup))`.** Either lower it explicitly at exit
   edges, or make dynamic variables a documented `cc`-only feature under
   `tur jit`.~~ **DONE (`4bde858fa`), and neither option was the answer -- see
   section 12.5.** The fall-through exit is one the expression emitter *can*
   see, so the pop is emitted explicitly there while the attribute is kept for
   the exits it cannot; an idempotent pop lets both fire. Corpus 1642 -> 1645,
   all ten `dynvar-*` fixtures green on both paths, dynamic variables not
   `cc`-only after all.
4. **Promote S2 ahead of J2**, per section 4.3. **Sized, not yet implemented --
   see section 13.** The preamble is 3,417 lines (not 3,847; 4.3 split the TU
   at an LCP that ran past the runtime into shared stdlib decls), 25 variants
   corpus-wide with one covering 89% of TUs, and **57% of total compile time**.
   The boundary a program actually reaches is **21 symbols at the median, 177
   as a corpus-wide union** -- small enough to be a hand-maintained header.
   The emitter now marks the region end so any consumer can split exactly.
5. ~~**Default to lazy generation** (`MIR_set_lazy_gen_interface`).~~
   **WITHDRAWN -- see 8.1 and 8.4.** Lazy generation has two independent
   defects at this pin: it is not re-entrant (8.1), and it miscompiles pthread
   entry functions even single-threaded (8.4.1). Full-corpus cost is 17
   fixtures, 16 of them session-types (8.4.4). Generate eagerly, and revisit
   only with a lock and a fix for the codegen bug.
6. ~~**Verify arm64 macOS MAP_JIT** before the `EXPERIMENTS[]` row lands.~~
   **Done, 2026-07-27 -- gate closed, see 8.1.** Replaced by three new items:
   (a) ~~move the atomic builtins into the host runtime instead of emitting them
   as text, and emit `_Thread_local` rather than `__thread`~~ **-- BOTH DONE
   (S1 and `94ead5062`); see section 14. Corpus unchanged at 1645 with an empty
   fixture diff -- the win is correctness, not coverage. Doing it separated out
   a third problem the two were masking: `stm-stress` and `gc-registry-growth`
   fail on TLS, not atomics -- c2mir accepts `_Thread_local` and then treats it
   as a plain global (14.3), which is J1 engine work**; (b) make
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
[docs/archive/history/jit-macos-full-corpus-extension-and-atexit.md](../archive/history/jit-macos-full-corpus-extension-and-atexit.md).

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
| ~~`conversion to non-scalar type requested`~~ | ~~2~~ **0** | **FIXED in `b61cdf578`** -- `TUR_APPLY<N>_T` expanded to `(A0)(a)`, a cast to a struct type when `A0` is an aggregate, which is not legal C. gcc accepts it; c2mir does not. The emitter now decides per argument: cast for scalars (load-bearing for the int64 <-> pointer direction), bare for aggregates (where it was provably a no-op). Corpus 1557 -> 1559. Archived at [docs/archive/history/jit-tur-apply-casts-to-aggregate-param-type.md](../archive/history/jit-tur-apply-casts-to-aggregate-param-type.md). |
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
[docs/archive/jit-reactor-fixtures-abort-under-mir.md](../archive/jit-reactor-fixtures-abort-under-mir.md).
The established fact is that the emitted preamble carries its own `static`
ucontext fiber runtime and uses a **JIT-generated function as a `makecontext`
entry point**; the same split exists on the `cc` path, where it works. Settling
it needs a backtrace, not another hypothesis.

The archive link is confirmed **exactly neutral** against the curated list:
1571/1680 both ways, zero fixtures changed outcome. Getting there surfaced one
more real defect: `libturi.a` does not contain `src/runtime/symbols.c`, so
`tur_sym_register` is absent from the archive. `tur build` never notices
because `libturt_runtime.a` supplies it.

(**Amended 2026-07-29 -- FIXED, and this paragraph's original explanation was
wrong.** It attributed the miss to an `ar` basename collision with
`src/compiler/symbols.c`. There is no collision: `runtime/symbols.c` was simply
absent from `TUR_CORE_SOURCES`, the only member of `TURT_RUNTIME_SOURCES` not
dual-listed there. Every *observation* behind the collision story was accurate
and every one of them holds under both explanations -- the repro could not tell
them apart and was written as though it had. One line of CMake fixes it; the
harness's compile-the-TU-in workaround is reverted, and the corpus is unchanged
at 1645 without it, which is what shows the fix replaces the workaround exactly.
Archived with the full post-mortem at
[docs/archive/history/libturi-symbols-basename-collision.md](../archive/history/libturi-symbols-basename-collision.md).)

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
  globals; ~~the six weak `tur_scheduler_*_st` FUNCTIONS carry the same hazard,
  are not value-copyable, and fold into the `__tur_static_init()` J1 work~~
  **-- dissolved in section 17: the whole module was dead code.**
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
  [docs/archive/history/persistent-map-cstr-keys-identity-compared.md](../archive/history/persistent-map-cstr-keys-identity-compared.md).
  **`self-recursive-carrier-struct-return` is also explained, in the opposite
  direction: a genuine upstream MIR miscompilation** -- a two-word by-value
  struct return inside an `if/else + goto-backedge` CFG (the emitted tail-loop
  shape) comes back as `{hi, hi}`; 12-line standalone-C repro, present at
  upstream master tip, each boundary condition verified by a one-line change.
  **Fixed in the rjungemann/mir fork** (`b79e3681`, root cause: `make_one_ret`
  merge targets alias when simplify canonicalizes a trailing `ret 0,0`); the
  spike pin now points at the fix commit. Corpus 1641 -> 1642. Archived:
  [docs/archive/history/mir-two-word-struct-return-goto-loop-miscompile.md](../archive/history/mir-two-word-struct-return-goto-loop-miscompile.md).
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
the `((cleanup))` decision, ~~`__tur_static_init()` (recommendation 2, still
open)~~ **-- done, section 12**, S2's prebuilt-preamble latency work, and the
three wrong-output fixtures.

## 12. S1b executed -- the attribute hazard, closed except for `cleanup`

Added 2026-07-29, x86-64 Linux, same harness and pin (`77a4f1209`). This is
recommendation 2 carried out.

### 12.1 What shipped

Seven emission sites carried `__attribute__((constructor))`. All seven now
register a plain `static void f(void)` into a per-TU table, and the emitter
closes the program with an explicit `__tur_static_init()` that `main` calls as
its first statement:

| Site | File | Band |
|---|---|---|
| `__sk_register` call frames (2 branches) | `emit_cps_ir.c` | REGISTRY |
| `__tur_e2reg_*` direct->CPS registration | `emit_cps_ir.c` | REGISTRY |
| `__tur_sym_seed` interned-symbol seed | `emit_core.c` | REGISTRY |
| `_dynvar_init_*` `pthread_key_create` | `emit_module.c` | KEYS |
| `__module_defers_init` / `__module_defers_<M>_init` | `emit_module.c` | ATEXIT |
| `__tur_module_def_init` top-level `def` initializers | `emit_module.c` | DEFS |

Two things the shape has to get right, neither of which the recommendation
mentioned:

**There are two cases an explicit call from `main` cannot cover.** Separate
compilation gives each TU its own initializers but only one TU has `main`, and
a `--shared` library has no `main` at all. So one `constructor` wrapper is
still emitted, and `__tur_static_init` is idempotent -- whichever path fires
first wins and the other is a no-op. The `cc` path is therefore unchanged in
behaviour, which is what keeps a 140-snapshot regen readable as pure addition.
Under the JIT, single-TU is the only shape J1 compiles, so the wrapper being
dropped costs nothing. **Multi-TU under `tur jit` is not covered by this
change** and should not be assumed to be; it needs the same treatment
`exports.manifest` gets in S3.

**Ordering stopped being the toolchain's problem and became ours.** Previously
these were N independent `.init_array` entries; now they are N calls in an
order this emitter picks. The bands above encode the dependencies -- keys
before anything reads a dynamic var, registries before any effectful indirect
call dispatches through them, and `__tur_module_def_init` last because it is
the only one that runs *user* code and so must see everything else in place.

### 12.2 The corpus does not move, and that is the expected result

| Sweep | Full corpus | |
|---|---|---|
| S1b emitter, normalizer rule 3 still active | 1642 / 1680 | 97.7% |
| **S1b emitter, rule 3 retired** | **1642 / 1680** | **97.7%** |

Both full runs, no sampling. The spike normalizer was already synthesizing the
constructor call sequence textually, so there was no fixture left for the
emitter to gain. The deliverable is the deletion: `normalize-c11-subset.py` is
down from three rules to two, and both survivors are hard parse errors rather
than silent drops.

One fixture differs between the two sweeps -- `stm-stress`, `signal-11` in the
first and `output-mismatch` in the second. It is not attributable: the same
binary on the same normalized file alternates between the two outcomes across
six consecutive runs. That is the shim-atomics class from 8.4.3 behaving as
already documented. **This is the third time in this document a failure-class
tally moved for a reason unrelated to the change under test** (31 -> 31 syntax,
3 -> 2 signals, 4 -> 5 mismatches reads like a regression and is not); the
fixture-by-fixture diff said "one fixture, and it is nondeterministic" in one
line. Section 11.6's rule holds.

### 12.3 The control: what the explicit call is actually worth

A coverage number that does not move proves nothing about whether the emitter
now does the rewriter's job. The direct test is to strip the
`__tur_static_init();` call from the normalized C, leaving only the attribute
c2mir discards, and compare:

| Fixture | with the call | call stripped |
|---|---|---|
| `dynvar-multi` | PASS | **SIGSEGV** |
| `dynvar-binding` | PASS | wrong output |
| `module-defer-basic` | PASS | wrong output |
| `cps-backend-effect` | PASS | PASS |
| `dynvar-nested` | wrong output | wrong output |

`cps-backend-effect` passing either way is worth noting rather than hiding:
its registry is not load-bearing for that particular program, so it was never
evidence for this fix in the first place. `dynvar-nested` is wrong in both
columns because its defect is `((cleanup))`, which S1b does not touch.

### 12.4 What is left of section 3.1

Of the five attributes in 3.1's table, `constructor` is closed here and
`cleanup` in 12.5 below; `unused` was always harmless. What survives is not
emitter-side work at all:

- **`packed`** and **`#pragma pack`** -- arrive from system headers and user
  inline-C, not from the emitter (9.3), so nothing in this section reaches
  them. They remain a `tur jit` ABI hazard on any program whose inline-C
  touches a packed struct.

### 12.5 `((cleanup))` -- recommendation 3, decided and lowered

The plan offered two ways out: lower `cleanup` at exit edges, or make dynamic
variables a documented `cc`-only feature under `tur jit`. Neither is quite what
the code wanted, and the third option is better than both.

`__attribute__((cleanup(_dynvar_pop_X)))` guards the binding frame in
`EX_DYNVAR_BINDING` (`emit_expr.c`), which the emitter already wraps in a plain
C block. The normal exit from that block is a fall-through the emitter can see,
so **the pop is emitted explicitly, in reverse declaration order, right before
the closing brace** -- and the attribute is *kept*, because it also covers exits
the expression emitter cannot see (a `return` or `goto` out of the block from a
`?`-propagation or a tail-loop backedge). To let both fire on the `cc` path,
`_dynvar_pop_X` became idempotent: it clears the guard pointer, so whichever
runs first does the work and the second returns immediately.

That is a strictly-additive change. The `cc` path keeps every exit edge it had,
gains nothing it did not have, and stays green; the JIT gains the fall-through
case, which is what every affected fixture actually needed.

| Sweep | Full corpus | |
|---|---|---|
| S1b, `cleanup` still attribute-only | 1642 / 1680 | 97.7% |
| **+ explicit scope-exit pop** | **1645 / 1680** | **97.9%** |

The fixture-by-fixture diff is exactly the three `dynvar-*` mismatches --
`dynvar-nested`, `dynvar-log-level`, `dynvar-thread-locale` -- and nothing else
moved. All ten `dynvar-*` fixtures now pass under both `cc` and the JIT;
`tests/run.sh` is 2399/0 with 140 snapshots regenerated.

**The honest remainder.** An early `return` or `goto` out of a dynamic binding
still pops on `cc` (via the surviving attribute) and still does not pop under
the JIT. No fixture in the corpus exercises that shape, which is why the
coverage number cannot see it -- so it is recorded here rather than inferred
from a green sweep. Closing it means the CPS/exit-edge emitter placing the same
call at each edge it generates, which is J1 work on a path this change does not
touch.

### 12.6 End state after S1b

| Stage | Full corpus | |
|---|---|---|
| J0 baseline | 1424 / 1680 | 84.8% |
| S1 + spike normalizer + host-symbol boundary (11.7) | 1631 | 97.1% |
| weak-config sync, builtin prototypes, MIR ret fix | 1642 | 97.7% |
| **S1b + `cleanup` lowering** | **1645** | **97.9%** |

The 35 remaining failures are, without exception, a recorded decision or a
filed report:

- **31 x** GNU constructs in user inline-C (all `httpd-*`) -- the plan's
  3.2-step-6 fallback to `cc`, working as designed.
- **1 x** `any-cast-mismatch-panic` -- panics by design; the sweep scores any
  signal as a failure.
- **2 x** `stm-stress`, `gc-registry-growth` -- ~~the shim-atomics hazard
  (8.4.3)~~ ~~re-diagnosed in 14.3 as TLS~~ -- **finally split in section 15:
  `stm-stress` was TLS (fixed, now a deterministic PASS), `gc-registry-growth`
  is MIR frame size vs. the default stack (open, a J1 sizing decision).**
- **1 x** `hamt-lowering-basic` -- the filed `^persistent` cstr-key identity
  bug, reproduced on the `cc` path with no JIT involved -- **FIXED in section
  22; now a native PASS**
  ([archived](../archive/history/persistent-map-cstr-keys-identity-compared.md)).

## 13. S2 sized -- the runtime boundary, measured exactly

Added 2026-07-29, x86-64 Linux, same harness and pin. Recommendations 4 and
6(c) ask for S2 to be re-scoped into J1 and re-measured. This is the
measurement; the implementation is J1 work.

### 13.1 4.3 split the TU in the wrong place, and so did the first re-run

Section 4.3 reported the fixed preamble as "3,847 lines, **byte-identical**
across `arith`, `hamt-basic`, and `cps-backend-effect`". That number is a
longest-common-prefix of three TUs, and an LCP does not stop at the end of the
runtime: those three programs also share several hundred lines of *stdlib
forward declarations* immediately after it. The figure is the runtime plus
whatever stdlib the sample happened to have in common.

Re-running the same LCP against all 1,928 emitted TUs gives **11 lines**, which
reads as "the preamble is barely fixed at all" and is just as wrong in the other
direction -- programs that gate a different preamble block diverge early, and
everything after the divergence point gets counted as program text. Both numbers
are artifacts of splitting by prefix agreement rather than by structure.

The emitter now closes `emit_runtime_preamble()` with an explicit marker:

```c
/* ==== tur: end of fixed runtime preamble ==== */
```

It costs one comment line per TU, is emitted unconditionally (including
`--shared` and every separately-compiled TU), and makes the split exact for the
spike, for J3's harness, and for anything S2 builds later. S2's stated
deliverable is "a named, documented symbol boundary"; a region has to be
delimited before its symbols can be. Corpus sweep after adding it: 1645/1680,
unchanged, as a comment should be.

### 13.2 With the exact split, 4.3's claim was right after all

| | Value |
|---|---|
| Preamble lines | **3,417** (median; 3,417 min, 3,689 max) |
| Distinct preamble variants across 1,928 TUs | **25** |
| TUs sharing the single most common variant | **1,711 (89%)** |

So the preamble is not literally byte-identical corpus-wide -- it is gated on
program features (`g_needs_hamt`, session types, and so on) -- but 89% of
programs get the *same* text, and the whole corpus is covered by 25 variants.
A content-keyed cache of prebuilt MIR modules is therefore viable with a
handful of entries, which is a stronger result than 4.3 claimed and a much
stronger one than the 11-line LCP suggested.

### 13.3 Latency, re-measured on the exact preamble

| Input | Lines | c2mir | link+gen |
|---|---|---|---|
| trivial `int main` + `<stdio.h>` | 2 | 10.0 ms | 1.7 ms |
| **fixed runtime preamble alone** | **3,417** | **97.8 ms** | **91.3 ms** |
| whole `arith` TU | 7,559 | 142.2 ms | 188.9 ms |
| whole `hamt-basic` TU | 8,075 | 139.9 ms | 199.3 ms |
| whole `cps-backend-effect` TU | 7,593 | 139.8 ms | 190.5 ms |

The preamble is **69% of c2mir time, 48% of generation, 57% of the total** for
`arith` -- against 4.3's 76% / 50%. Same conclusion, slightly smaller share.

Absolute times here run ~1.5x 4.3's on the same fixtures at the same line
counts, which is a property of the machine this session ran on, not a
regression: the ratios are what carry across sessions and the ratios moved by a
few points. Do not compare the millisecond columns of 4.3 and 13.3 directly.

### 13.4 The boundary is 21 symbols for a typical program, 177 corpus-wide

The number that actually sizes S2 is not the preamble's size but how much of it
a program *reaches* -- that set is the `MIR_load_external` table, and the plan
says so ("the JIT's `MIR_load_external` table *is* that list").

Splitting every TU at the marker, taking the symbols the preamble defines
(`nm --defined-only`, 340 of them) and intersecting with the identifiers
appearing after the marker:

| | Value |
|---|---|
| Symbols the preamble defines | 340 |
| Referenced by the program half -- median | **21** |
| -- p95 | 29 |
| -- max | 46 |
| **Union across all 1,928 TUs** | **177** |
| Defined but referenced by no program in the corpus | **163** |

Two things follow. **Nearly half the preamble is internal to itself**: 163 of
340 symbols are never named by any generated program, so they are pure
runtime-private code that has no business being recompiled per program and no
business appearing in a boundary header either. And **the boundary is small and
stable** -- 21 symbols are referenced by more than 90% of TUs, and the widest
program in the corpus reaches 46. A hand-maintainable header is a realistic
artifact at that size; it would not have been at 340 or at the thousands the
"3,847 lines" framing suggests.

This is measured by identifier occurrence in the post-marker text, so it is an
upper bound on genuine references (a symbol named only in a comment or a string
would count). It is not an upper bound on what S2 must *resolve*: a program also
reaches libc and, under the JIT, whatever the host runtime supplies by address.

### 13.5 What J1 should take from this

- The `--runtime=lib` machinery (`apply_runtime_lib_mode`, `src/main.c`) already
  swaps runtime sources for the archive on the `cc` path, and `g_rcgc_from_archive`
  already gates a preamble block between "define" and "declare". S2 is an
  extension of an existing pattern, not a new mechanism.
- Skipping the preamble saves ~57% of compile time on a typical program. On
  Apple Silicon, where 8.3 found the JIT at parity with `cc`, that is the
  difference between a feature with no latency argument and one with a clear
  one.
- The 25-variant clustering means a prebuilt-module cache keyed on preamble
  content is cheap. It does not need to be perfect: a miss falls back to
  compiling the preamble, which is today's behaviour.

## 14. Recommendation 6(a) -- atomics moved, and the TLS half separated out

Added 2026-07-29, x86-64 Linux, same harness and pin (`94ead5062`).
Recommendation 6(a) bundles two things: emit `_Thread_local` rather than
`__thread`, and move the atomic builtins into the host runtime. The first
shipped with S1. This is the second -- and doing it separated a third problem
that was hiding behind both.

### 14.1 What shipped

The preamble emitted 18 literal `__atomic_*` builtins. c2mir implements none of
the family, so the spike shimmed them to plain loads and stores -- correct only
for single-threaded programs, and the shim said so in its own header.

They now route through a `TUR_ATOMIC_*` macro layer:

- under `__GNUC__`/`__clang__`, expands to exactly the builtins emitted before,
  so the `cc` path is behaviourally identical and keeps the inline atomic on the
  STM commit path;
- otherwise, calls `src/runtime/tur_atomics.c` -- compiled by `cc`, resident in
  the host, resolved by address exactly as `hamt.c` already is. This is plan
  section 3.2 step 4 applied to the one part of the runtime that could not
  survive c2mir at all.

c2mir predefines neither macro. That was verified with a probe rather than
inferred from 9.2's `__extension__` note, which is the same claim from a
different direction.

Memory orders are not threaded through the host functions -- all seven are
seq_cst, a strengthening of every order the preamble requests (relaxed,
acquire, release, acq_rel), so no program can observe a behaviour the requested
order would have forbidden. Passing the order would cost a switch per call
(GCC requires a compile-time-constant order), on a path only ever taken by a
front end that has no builtins to be fast with.

### 14.2 The corpus does not move, and the diff is empty

| Sweep | Full corpus | |
|---|---|---|
| before | 1645 / 1680 | 97.9% |
| **after** | **1645 / 1680** | **97.9%** |

Fixture-by-fixture against the pre-change sweep: **zero fixtures changed
outcome**. The win is correctness -- the preamble's atomics are real under the
JIT instead of faked -- not coverage. Recording that plainly matters, because
the tempting write-up ("atomics fixed") implies a number that did not move.

Getting to that empty diff took two wrong turns, both caught by measurement:

- **The fallback declarations were unreachable.** They are spelled in `uint64_t`
  and were emitted with the macro block, which precedes every `#include`. Under
  a GNU compiler the fallback branch is never compiled, so nothing shows; under
  c2mir it is a parse error on every program. Moved after `<stdint.h>`.
- **Deleting the shim's atomic defines cost 13 fixtures** (1645 -> 1631), every
  one an `unresolved import: __atomic_*` originating in `stdlib/atomic.tur`,
  `stdlib/future.tur`, or fixture inline-C. The shim was covering *inline-C*,
  which the emitter does not own, as well as the preamble. Restoring it, I also
  dropped `#define __thread` (same mistake: it covered user inline-C too, since
  `thread-local-basic` writes `static __thread` in a C block) and added an
  `__atomic_exchange_n` that returned the old value without storing the new one.
  Both surfaced as a 1643 naming two fixtures.

The shim's remaining atomics are still not atomic, and are now correctly scoped:
they exist for inline-C only. `tur jit` must either take the step-6 `cc`
fallback for inline-C that uses them, or `stdlib/atomic.tur` must route through
host functions the way the preamble now does.

### 14.3 `stm-stress` was never an atomics failure -- it is TLS

With real atomics in the preamble, `stm-stress` still loses updates (4000
expected; 3930, 1917, 97 across runs, sometimes SIGSEGV). The cause is one line
up from where 6(a) was looking:

```c
static TUR_THREAD_LOCAL STM_Transaction *__stm_current_tx = NULL;
```

c2mir parses `_Thread_local` and then warns **"Thread local is not
implemented"** -- 10 times per program -- and treats the variable as an ordinary
global. All 8 worker threads therefore share one transaction descriptor. 3.2
predicted exactly this ("behaves as a plain global, which is correct
single-threaded and wrong the moment a fixture spawns"); what is new is that it
is now the *only* thing standing between `stm-stress` and a pass, with the
atomics no longer masking it.

No emitter change fixes this. The 10 thread-local variables in the preamble
(`__stm_current_tx`, the handler chain and panic flag, the current fiber and its
cancel flag, the thread state and its cancel `jmp_buf`, the MT scheduler
pointer) would have to move into the host behind accessor functions backed by
`pthread_getspecific` -- the pattern dynamic variables already use -- or `tur
jit` documents itself as single-threaded until MIR implements TLS. That is J1
engine work and is deliberately not attempted here.

The same reading was applied to `gc-registry-growth` at the time this section
was written -- and half of it did not survive section 15: with real TLS in
place that fixture STILL crashes, because it is a 20,000-deep non-tail
recursion whose MIR-gen frames outgrow the default stack (15.3). So the honest
status of 12.6's "2 shim-atomics casualties" line is: neither was atomics;
one (`stm-stress`) was TLS, one (`gc-registry-growth`) is frame size.

## 15. Multi-threading retained -- TLS routed host-side, and what it flushed out

Added 2026-07-29, x86-64 Linux (`dbc2c0bfd`, MIR fork `41ff4d94`). Section
14.3 ended with a choice: move the preamble's thread-locals into the host, or
document `tur jit` as single-threaded. **The owner's decision is that the JIT
retains multi-threading**, so this is the first option, executed.

### 15.1 The mechanism

The preamble declares 11 thread-local variables (STM's current transaction,
the handler chain and panic flag, the shift/reset context, the current fiber
and its cancel flag, thread state and its cancel `jmp_buf` and validity flag,
the MT scheduler pointer, and the runtime type-value scratch). All went
through `emit_rt_global` already, so one sibling helper (`emit_rt_tls`)
converts them all:

- **GNU-family cc:** the identical `TUR_THREAD_LOCAL` variable as before --
  the compiled `cc` path is unchanged in both text-shape and behaviour.
- **Any other front end:** the variable is not declared; the *name* becomes an
  object-like macro, `#define __stm_current_tx
  (*(STM_Transaction **)tur_tls_stm_current_tx_ptr())`. The accessor lives in
  `src/runtime/tur_tls.c`, compiled by a real cc, holding a genuine `__thread`
  slot and returning the calling thread's instance by address. Same
  host-residency pattern as `tur_atomics.c`, applied to state.

Slots are `void*`/`int`/`bool`/`int64_t`/`jmp_buf`, so the host knows nothing
of preamble-private structs; the macro restores the precise type at each use
site. Safety of the object-like macro was *measured*, not assumed: across all
1,928 emitted TUs, none of the 11 names ever appears as a struct member (where
a macro would also expand after `.`/`->`).

Result: `stm-stress` prints a deterministic 4000 on 10 consecutive runs
(previously 3930 / 1917 / 97 / SIGSEGV across runs), and c2mir's "Thread local
is not implemented" warning count drops from 10 per program to 0. Corpus
1645 -> 1646 (98.0%), fixture diff exactly `{stm-stress}`, zero regressions.

### 15.2 Second engine bug: `try_spilled_reg_mem` overruns a 2-entry array

The first sweep after the TLS change broke `module-spec-same-module` with
`*** stack smashing detected ***` -- a regression the fixture diff caught
immediately. The backtrace lands in MIR's register allocator
(`rewrite_insn`), and an ASan build of the engine pinned it:

```c
int n = 0, op_nums[MAX_INSN_RELOAD_MEM_OPS];   /* == 2 */
...
    insn->ops[i] = mem_op;
    gen_assert (n < MAX_INSN_RELOAD_MEM_OPS);
    op_nums[n++] = i;
```

`try_spilled_reg_mem` replaces *every* occurrence of a spilled register in one
instruction with its stack-slot form. `mul v, v, v` -- which coalescing
produces from `r = r * r`, i.e. the fixture's generic `square` -- has the same
register in **three** operand positions; the third write lands in
`op_nums[2]`, past the end. The TLS *accessor call* is what exposed it:
a call in the panic guard raised register pressure enough to spill `v`, which
this path had never seen before. The trigger was ours; the bug is upstream's,
present at master tip.

**Fixed in the rjungemann/mir fork** (`41ff4d94`, on the same branch as the
`make_one_ret` fix): on a third occurrence, undo the replacements and fall
back to the ordinary reload path -- the same thing the function already does
when `target_insn_ok_p` rejects. Verified by ASan (clean, correct output) and
the corpus (one intended fix, zero regressions). NOT filed upstream, per the
same owner decision as the first fix.

### 15.3 Correction: `gc-registry-growth` was never TLS either

14.3 filed both remaining signals under TLS. Half right. `gc-registry-growth`
still crashed with real TLS -- because it is a **20,000-deep non-tail
recursion**, single-threaded, whose MIR-gen frames are simply bigger than
gcc's: it dies at the default 8 MB stack and passes verbatim at 16 MB
(`ulimit -s 16384`). Bounds: gcc's frame for this function fits 20,000 deep in
8 MB (<= ~419 bytes); MIR's does not, but fits in 16 MB (< ~840 bytes). So this
is **code quality / stack sizing**, not correctness.

**Resolved as a sanctioned stopgap (owner decision, 2026-07-29): "any size
temporarily is fine, but retain the stackless nature of the runtime in the
long run."** The harness now runs the JIT'd entry on a thread with an
explicitly sized stack (64 MB default, `TUR_JIT_STACK_MB` to override) --
which is what a real `tur jit` would do -- and `gc-registry-growth` passes.
Probing the threshold through that override tightens the bound: the fixture
dies at 11 MB and passes at 12, so MIR's frame for this function is ~590
bytes against gcc's <= ~419 -- a ~1.4x factor, not the 2x the first bounds
suggested.

The long-run direction the decision names matters more than the number. The
tur/turi runtimes were deliberately rewritten stackless -- turi as a
work-stack machine, the compiled path's effect/CPS code on heap-allocated DK
continuations -- and that architecture survives MIR untouched
(`cps-backend-effect` is an exit-criteria pass; the DK trampoline needs no
shim). What grows under MIR is only the *direct path's* plain C frames, which
are stackful under `cc` too. So the eventual fix is MIR frame-size work
(fork territory) or routing deep direct recursion through the existing
stackless machinery -- **not** ever-bigger stack constants. The stopgap's
comment in the harness says exactly this, so the constant cannot quietly
become the design.

That is the third re-diagnosis of this pair of fixtures (8.4.3: shim atomics;
14.3: TLS; now: one TLS + one frame size), and each step was driven by
removing the masking layer the previous diagnosis named. The lesson for J3 is
the same one 11.6 recorded: a failure class named from its symptom
("signals") is a bucket, not a diagnosis.

### 15.4 The cache-variable trap almost shipped an unpatched engine

Repointing `TUR_MIR_GIT_TAG` in `cmake/mir.cmake` does nothing to an existing
build directory: `set(... CACHE ...)` never updates an existing cache entry,
and `rm -rf build-jit/_deps` re-clones from the **cached** repo/tag, not the
file's. The rebuilt spike was briefly pure upstream `a8ab7c31` -- both fork
fixes silently absent -- caught only because `git -C _deps/mir-src log` was
checked rather than trusted. (It also revealed that the earlier `make_one_ret`
verification had been running against a hand-patched `_deps` tree with the
cache still pointing at upstream -- right code, misleading provenance.)
`cmake/mir.cmake` now documents the trap; the rule is: after any repoint,
configure with `-DTUR_MIR_GIT_TAG=...` or a fresh dir, and verify HEAD in
`_deps/mir-src` before believing any number.

### 15.5 End state

| Stage | Full corpus | |
|---|---|---|
| S1b + cleanup lowering (12.6) | 1645 / 1680 | 97.9% |
| 6(a) atomics (14) | 1645 | 97.9% |
| **TLS host routing + RA fix (fork pin `41ff4d94`)** | **1646** | **98.0%** |
| **sized entry stack (15.3 stopgap)** | **1647** | **98.0%** |

The remaining 33: 31 user-inline-C fallbacks (by design), 1 by-design panic
(`any-cast-mismatch-panic`), 1 filed `^persistent` key bug
(`hamt-lowering-basic` -- **since fixed, section 22**). **The sweep now
contains zero open engine or emitter items.**
**Multi-threaded programs are now first-class under the JIT**: STM commits,
scheduler cancel flags, and select's winner CAS run on real atomics and real
per-thread state. What J1 still owes multi-threading specifically:
concurrent-safe (or serialized) lazy generation (8.1) -- the scheduler
weak-function fold dissolved as dead code (17).

## 16. S1 completed -- 13,730 `__auto_type` sites down to 26

Added 2026-07-29 (`5f9418f86`). Section 11.2 left S1 with an honest debt: the
emitter work "does not delete the normalizer", because the residue --
indirect calls -- was inferred on purpose. This section pays the debt down to
one named family. The motive is J1's entry condition: an in-compiler
`tur jit` cannot run a Python rewriter between emit and c2mir, so the emitted
C has to be subset-clean as emitted.

### 16.1 The census, then the fixes

Every one of the 1,928 emitted TUs still carried `__auto_type` -- 13,730
sites. Shape census before fixing anything (the same fixture-by-fixture
discipline 11.6 institutionalized, applied to emitted text):

| Shape | Sites | Fix |
|---|---|---|
| cast-fn-ptr call | 6,396 | builder note (ground truth) |
| thunk-typedef call | 4,385 | builder note (ground truth) |
| `ctor_*` monomorphs | 2,289 | record at type registration |
| `INT64_C` | 308 | exact read by name |
| direct-name lookup misses | 218 | cps->direct sig lookup + (residue) |
| member `.fn` dispatch | 134 | region notes at result wraps |

Three mechanisms:

- **Registration-time recording** for monomorph ADT ctors. The renderer
  (`emit_registered_adt_app_rec`) runs at final program assembly, after every
  body, so recording there is too late by construction -- the same too-late
  shape 11.1 found twice (`emit_sig_reset` placement; extern-c pre-pass).
  `type_register_adt_app` fires the moment a body first names the type, which
  is always at-or-before the first ctor call.
- **A builder-to-hoist note** (`EmitCtx.call_ret_note`): each indirect-call
  builder hands the panic-hoist the same `ret_c` string it just spelled into
  the call text's own cast or thunk typedef. Protocol matters more than the
  field: set as the LAST thing before returning the composed string, captured
  and cleared unconditionally by `emit_value` after every dispatch -- so a
  note from a void/never call (whose hoist is skipped) can never leak onto a
  later, unrelated call.
- **Two anchored exact reads** at the hoist, as the last resort: `((RET (*)`
  and `(T)(expr)` with T restricted exactly as the normalizer's CAST_RE was
  (primitive spellings or trailing `*`), so `(f)(x)` cannot be misread.
  These are the spike normalizer's two blessed rules -- "reads off text the
  emitter itself generated" -- ported to the one place they are needed. They
  exist because one builder's note structurally cannot survive:
  `emit_call_name` composes the dict-vtable dispatch head, and the argument
  emissions that follow it clear any note it could set.

One measurement-caught mistake worth its line: the cast read first required
TWO leading parens and matched nothing (71 residue, unchanged shape). The
hoist's own printf adds the outer paren pair, so a cast wrap arrives as
`(T)(expr)` with one. 71 -> 26 after the fix -- the census caught in one
re-emit what reading the code had not.

### 16.2 What remains, and why it is structural

The 26 survivors sit in 11 TUs, all van-laarhoven lens fixtures, all direct
calls to `<consumer>__lens_<hash>` / `<lens>__mono_<hash>` clones. Their ABI
specs are minted in a dedicated block AFTER the main emit loop has already
emitted `main`'s body -- so at the moment the call site consults the table,
the spec (and its forward declaration, which is what records) does not exist
yet. This is the ctor problem again, but the fix is not "add a record": the
name is composed on the fly at the call site and the spec is minted from
usage collected during the loop, so closing it means moving lens-spec minting
ahead of body emission. Deferred at first for exactly one session (**closed in 16.4**): moving spec
minting stayed too risky, but the three redirect sites each know `e->type`,
and handing its c-name to the hoist -- then *verifying* the derivation
corpus-wide rather than trusting it -- turned out to be both safe and
checkable.

### 16.3 Verification

- `tests/run.sh` 2399/0 with 140 snapshots regenerated -- the typed temps are
  the bulk of the diff. `run-turi.sh` 1657/0.
- Corpus 1647/1680 with an **empty** fixture diff against the pre-change
  sweep: naming the types changes no behaviour on either path. (A wrong
  read/note would have: these declarations are live on the `cc` path too, so
  the suite is the control that the "exact read" claim is actually exact.)
- The normalizer is now needed for: the `__tur_include__` hoist (which is
  `tur build`'s own in-process post-pass, not a subset fix), and the 26 lens
  sites. Its `__auto_type` machinery is inert for the other 1,917 TUs.

### 16.4 Zero, verified -- and the census trap, sprung twice more

Added later the same day (`3e6b26990`). The lens residue is closed at the
three redirect sites (`note_call_ret(ctx, emit_type_c_name(ctx, e->type))`),
and the derivation is checked rather than believed:
`tools/jit-spike/verify-temp-types.py` compares every typed direct-call temp
in every emitted TU against the callee's own declaration in the same TU --
**247,487 temps across 1,928 TUs, 0 mismatches**. Two accepted differences
are explicit in its code, not silent: `INT64_C`/`UINT64_C` (typed by the
macro's definition), and same-width signedness (`rc_strong_count` declares
`uint64_t` in C and `:int` in Turmeric; the side table records the
language-level type, the conversion is value-preserving, and the pre-S1
`__auto_type` temp fed the same int64 contexts).

The verifier's own first runs re-learned two of the retired normalizer's
oldest lessons -- caught by output, not review. Its declaration regex let the
lazy type group split an identifier, so the statement `if (f(x))` parsed as a
declaration of `f` returning `i`: the exact `printf -> sn` bug PROTO_RE's
mandatory separator fixed in J0's first week, reproduced verbatim in a new
tool. And it flagged `bool t = (INT64_C(1)) == (INT64_C(2))` by taking the
comparison's first token as the initializer's callee. Both were verifier
bugs, and both had the same shape as bugs this document already recorded --
tooling regresses toward known failure modes when the lesson lives in prose
instead of in the check.

Then the retirement itself sprang the trap a third time. Stripping the
normalizer to hoist-only dropped the corpus **1647 -> 1629**: 18 fixtures
failed on "braces around scalar initializer" -- pointer compound literals
(`(const char *){0}`) from three emitter sites S1's scalar-zero fix missed (a
ctor default-argument site, `emit_core`'s option default, `emit_fns`'
aggregate-return panic path). The `__auto_type` census counted ONE rule's
sites and the whole file was retired on it, while the scalar-zero rule was
still quietly fixing 18 TUs. All three sites now route through
`emit_c_zero_of`; corpus back to 1647.

### 16.5 End state: the emitted C is c2mir-clean as emitted

| Sweep | Full corpus | |
|---|---|---|
| full normalizer (baseline M) | 1647 / 1680 | 98.0% |
| **hoist-only normalizer (O)** | **1647 / 1680** | **98.0%** |

Fixture diff between the two: **empty**. `normalize-c11-subset.py` now
contains exactly one transformation -- the `__tur_include__` hoist, which is
not a subset fix but a replay of `tur build`'s own in-process post-pass that
bare `tur emit-c` does not run. A real `tur jit` therefore needs **no
external rewriting at all**: plan section 3.2's step 2 (run the existing
post-passes on the buffer) is the whole story between `emit_program` and
`c2mir_compile`. This was S1's original exit criterion, one prediction
("deletes the normalizer") finally made true -- two sections and three
mechanisms after 11.2 showed the prediction was premature.

## 17. The scheduler weak-function hazard was in dead code

Added 2026-07-29. Sections 11.7 and 15.5, the plan's J1 bullet, and the
harness's own boundary note all carried the same J1 work item: the six weak
no-op `tur_scheduler_*_st` functions in `src/async/scheduler_common.c` are a
link-time handshake that cannot cross the JIT boundary (host direct calls
cannot be re-bound to MIR code the way `tur_closure_headers_enabled`'s value
was copied), so they "fold into the `__tur_static_init()` work" -- the
expected fix being a registration API where the program hands the host its
function pointers at startup.

Sitting down to build that, the first question was *when does the host call
these* -- and the answer is never:

- No emitted TU defines the strong `tur_scheduler_*_st` implementations the
  weak stubs claim to defer to ("real implementations are in generated
  output" -- 0 of 1,928 TUs contain them; the comment described a codegen
  that no longer exists).
- The only calls to the weak functions are inside `TurSchedulerCommonST`'s
  vtable methods, whose sole constructor is `tur_scheduler_common_new` --
  which has **zero callers** in `src/`, `stdlib/`, and all 1,928 emitted TUs.
- Every other export of the module (`_current`, `_set_current`, `io_wait`,
  `io_signal`) is equally unreferenced. The real fiber/scheduler machinery
  (`fiber.c`, `scheduler.c`, and the emitted preamble's own copy) never
  touches it.

The module is deleted -- `scheduler_common.c`, its header, and its
`TUR_CORE_SOURCES` row. Verification: full rebuild of `tur` and the spike,
suite 2399/0, corpus 1647/1680 with an **empty** fixture diff, which is what
"nothing reached it" predicts.

The J1 work item this dissolves was carried through four documents for three
sessions, always phrased as future engine work, never once checked for
reachability -- because the weak-symbol *mechanism* was real (the reactor
abort proved that for the config global) and the six functions pattern-matched
it perfectly. The lesson is 11.6's again, from a new angle: a hazard inherited
from a correct analysis of a NEIGHBORING defect still needs its own
reachability check before it becomes a plan line.

## 18. J1 landed -- `tur jit <file>` exists

Added 2026-07-29. Phase J1's deliverable, per plan sections 3.1-3.2:

- **Two gates, both required.** Build-time: `-DTUR_JIT=ON` vendors MIR
  (cmake/mir.cmake, same fork pin as the spike) and compiles
  `src/jit_engine.c` into `tur` with `ENABLE_EXPORTS` so the resolver can see
  the linked-in runtime; a default build carries no fetch and no dependency.
  Run-time: the `jit` EXPERIMENTS[] row (introduced 0.32.2, expires 0.36.0,
  `g_opt_jit`), so `tur jit` errors out without `--enable=jit` and fires
  TUR-W0060 with it. Each gate reports its own absence usably.
- **The pipeline is plan 3.2 verbatim.** cmd_jit reuses cmd_build's front
  half (`compile_to_c` -> `hoist_tur_include_directives` ->
  `scan_autolink_markers`, all in memory), then `tur_jit_execute`: autolink
  `-l` entries dlopen'd RTLD_GLOBAL, c2mir over the buffer, `MIR_link` with a
  dlsym(RTLD_DEFAULT) resolver + the builtin-shim table + atexit
  interception, eager generation (recommendation 5), weak-config sync, entry
  on a sized-stack thread (15.3), `*args*` built from everything after `--`.
- **Step-6 fallback is wired and observed.** Any engine-level failure prints
  TUR-W0070 and delegates to cmd_run whole (it never reads argv[1]); a GNU
  range-initializer probe compiles c2mir-rejected inline C through cc and
  prints the right answer with rc 0.
- **What the engine deliberately does NOT carry:** the spike shim's
  `#define __thread` and fake non-atomic `__atomic_*` lowerings. Those exist
  to squeeze fixture coverage out of inline-C the emitter does not own;
  shipping them would trade a clean compile error for silent corruption under
  spawn. Inline C that uses them takes the fallback.

Verified on the J1 build: the three J0 exit-criteria fixtures plus
`stm-stress` (deterministic 4000 -- real atomics, real TLS, 8 threads),
`dynvar-nested`, `module-defer-basic`, `sym-dynamic`, and
`gc-registry-growth` all match expected output under `tur jit`; args
passthrough matches the cc path byte-for-byte; the default build's suite is
2399/0 untouched.

Latency, cold cache, `arith` end to end: **jit ~300 ms vs cc ~330-470 ms**.
The shared Turmeric front end dominates both, which is 8.3's Apple-Silicon
finding showing up on Linux at whole-command granularity: S2's prebuilt
preamble remains the latency story, now measurable directly on `tur jit`.

Known limits, recorded not hidden: single-file mode only (project mode and
`tur repl --jit` are J2); the c2mir "unknown pragma" warnings still print
(cosmetic); the `-lturi` SDK anchoring probe hardcodes `build/` so a
`build-turjit/`-located binary cannot link fallback programs that autolink
`-lturi` -- pre-existing (`./build/tur run` has the same limit in-tree), noted
for the install-layout work.

### 18.1 The product swept the corpus, and found its own first bug

`tools/jit-spike/sweep-turjit.sh` drives the REAL `tur jit` subcommand over
the same 1,680 fixtures as the spike sweeps -- no emit-c step, no normalizer,
no shim; the pipeline a user runs. Fallback is a first-class outcome rather
than a failure.

The first run found a genuine cmd_jit bug the eight smoke fixtures could not:
13 fixtures whose stdlib/fixture inline-C uses the GCC `__atomic_*` builtins
died with EMPTY output instead of falling back -- because **MIR's default
error handler exits the process** from inside `MIR_link` on an unresolved
import, killing `tur` before the step-6 fallback could run. The engine now
installs an error handler that unwinds back to `tur_jit_execute` via longjmp
(deliberately leaking the half-initialized context: tearing it down from an
undefined intermediate state is how a fallback becomes a crash), and the
caller takes the cc path.

| Outcome | Count | Reading |
|---|---|---|
| **jit-native PASS** | **1,633** | ran in process, output correct |
| fallback-pass | 14 | inline-C c2mir rejects (13 x `__atomic_*`, 1 x `__thread`); TUR-W0070 + correct output via cc |
| fallback-env | 31 | `httpd-*`: fallback fired but this checkout cannot link `-lturi` (pre-existing; `./build/tur run` has the same limit) |
| output-mismatch | 1 | `hamt-lowering-basic` -- the filed `^persistent` key bug |
| FAIL (signal-6) | 1 | `any-cast-mismatch-panic` -- panics by design |

1,633 + 14 = 1,647: exactly the spike's number, with one honest difference in
its favor -- the spike "passed" the 13 atomics fixtures through its fake
non-atomic shim lowerings, while the product compiles them through cc and
runs them on real atomics. The product is strictly more correct than the
spike that validated it.

Also fixed while writing the sweep: probing the binary with
`... | grep -q` under `set -o pipefail` SIGPIPEs the probed process on
match, so a successful capability check read as failure. Capture, then
match.

## 19. S2 architecture proven -- the runtime split works end to end

Added 2026-07-29. J2 requires S2, and S2's implementation shape had never
been settled beyond "a named symbol boundary." This section settles it with a
working proof and enumerates every seam the production version must handle.

### 19.1 Family-by-family is the wrong shape; two discoveries first

The JIT-path preamble (2,946 lines -- smaller than 13.2's number, see below)
holds 2,113 definition lines across 198 functions with **no dominant family**:
scheduler 244, DK/CPS 230, select 169, panic 166, fiber 159, sessions 159,
STM 143, timer 103, futures/io ~150. At ~29 us of c2mir per line, each family
is worth ~5-7 ms -- a dozen emitter surgeries with a dozen ABI seams for
wins that small individually. S2 must move the region wholesale.

Discovery one, found by reading before building: **rc/GC host-residency has
been live on the JIT path all along.** cmd_jit sets `g_emit_for_link`, so
DEDUP-4b's `resolve_rcgc_from_archive()` fires, finds `libturt_runtime.a`,
and emits the rc/GC family as declarations resolved by address into the host
-- which the 1,633-fixture product sweep validated without anyone noticing.
That is why the JIT-path preamble is 2,946 lines against 13.2's 3,417: the
554-line rc/GC block is already out. S2's mechanism is DEDUP-4b's mechanism,
scaled to the whole region.

### 19.2 The proof

`tools/jit-spike/s2-split-proof.py` splits an emitted TU at the preamble
marker into (a) a runtime impl -- de-static'd, compiled ONCE by gcc into a
`.so` -- and (b) a declarations-only region spliced ahead of the program half
for c2mir. The spike harness gained `TUR_JIT_PRELIB`, a resolver-priority
hook standing in for the production arrangement. Eight fixtures spanning
every hard subsystem -- `arith`, `hamt-basic`, `cps-backend-effect`,
`stm-stress` (8 threads), `dynvar-nested`, `module-defer-basic`,
`gc-registry-growth` (20k-deep recursion), `self-recursive-carrier-struct-return`
-- **all pass** with the runtime host-resident.

Latency, best of 5, `arith`:

| | c2mir | link+gen | total |
|---|---|---|---|
| full TU (status quo) | 92.5 ms | 141.0 ms | 233.5 ms |
| **split (program half only)** | **77.6 ms** | **66.6 ms** | **144.2 ms** |

**38% of engine time gone**, and the split is lopsided in an instructive way:
generation halves (the runtime's 198 functions no longer generate per
program) while c2mir drops only 16% (the declarations still parse; they just
compile no bodies). For J2's `(reload)` loop this is the per-reload saving,
and the `.so` compile is one-time.

### 19.3 The seams, each found by a failing fixture

1. **`static inline` loses its definition when de-static'd** -- plain C99
   `inline` at external linkage emits no standalone symbol, so the `.so`
   lacked `tur_frame_init`. Production: emit these `extern inline` +
   declaration, or drop `inline` in the runtime TU.
2. **TLS must have exactly one storage.** Giving the `.so` its own `__thread`
   variables plus accessor overrides failed silently: dlsym(RTLD_DEFAULT)
   prefers the EXECUTABLE's exports over a preload, so the program half bound
   libturi's slots while `.so` code used its own -- divergent state,
   `stm-stress` lost every increment. Fix: the impl routes the 11 TLS names
   through the HOST accessors too (the emitted `#else` branch, applied to the
   runtime TU). One storage, owned by `tur_tls.c`.
3. **The host already carries a diverged second runtime.** `tur`/the harness
   export `dk_prompt`, `tur_atomically`, `tur_stm_current_tx` from libturi's
   own `cps_rt.c`/`stm.c` -- different vintages of what the preamble emits.
   De-static'ing the preamble made its names collide: `.so`-internal calls
   were preempted by host implementations with different layouts (CPS
   SIGSEGV), and the program half bound a mix (STM answered 0). Proof-scale
   fix: `-Wl,-Bsymbolic` self-binds the `.so`, and the resolver consults the
   runtime lib before the process. **Production consequence, and it is the
   big one: the generated runtime library must BE the runtime** -- built into
   `tur` in place of the duplicated `cps_rt.c`/`stm.c`/fiber TUs, not
   alongside them. That also finally reconciles the divergence the reactor
   bug came from.

### 19.4 What production S2 is, concretely

1. An emitter mode producing the two artifacts from the same marked region:
   the feature-complete runtime TU (all gates on, statics externalized, TLS
   via host accessors) and the declarations region. Generated-and-committed
   like `stdlib/docstrings.tur`, regenerated when the preamble changes, with
   a content-hash guard: emit-time hash mismatch falls back to full-preamble
   emission -- never wrong, just slower.
2. The runtime TU compiled into `tur` (replacing libturi's duplicated copies)
   and into `libturi.a` for the cc path's `--runtime=lib`.
3. cmd_jit emits declarations-region + program half when the hash matches.

The proof script and the `TUR_JIT_PRELIB` hook stay in-tree so the macOS
re-validation can replay this end to end.

## 20. arm64 macOS re-validation of J1 + S2 -- three real defects, none of them the JIT's

Added 2026-07-29 on an Apple M2 (Darwin 27.0.0, Apple clang 21.0.0), `tur`
v0.32.2, MIR pin `41ff4d94`, `-DTUR_JIT=ON` Release build. Everything from
section 11 onward -- S1, S1b, 6(a)/TLS, J1, and section 19's S2 proof -- had
only ever run on x86-64 Linux. This is the macOS replay section 19.4 reserved.

**Verdict: the architecture holds on Apple Silicon.** The S2 split proof passes
8/8, the J1 engine sweeps the corpus within 3 fixtures of the Linux number, and
every one of the three genuine failures traces to a **pre-existing Turmeric
defect that the `cc` path was getting away with by luck** -- not to MIR, not to
Apple Silicon, and not to anything J1 built.

### 20.1 Smoke and full-corpus results

The eight subsystem fixtures section 18 verified on Linux all pass natively,
with no fallback: `arith`, `hamt-basic`, `cps-backend-effect`, `stm-stress`
(deterministic 4000 over 5 runs -- real atomics, real TLS, 8 threads),
`dynvar-nested`, `module-defer-basic`, `sym-dynamic`, `gc-registry-growth`.

Note `module-defer-basic` **passes**, which retires the open worry in 9.4 that
macOS's resolvable `atexit` left those fixtures SIGSEGV-ing at exit. J1's
`atexit` interception handles it.

`tools/jit-spike/sweep-turjit.sh`, same 1,680 eligible fixtures as Linux:

| Outcome | macOS | Linux (18.1) |
|---|---|---|
| **jit-native PASS** | **1,617** | 1,633 |
| fallback-pass | 27 | 14 |
| **correct (native + fallback)** | **1,644** | **1,647** |
| fallback-env (`-lturi`, pre-existing) | 31 | 31 |
| output-mismatch | 2 | 1 |
| FAIL | 3 | 1 |

The whole delta reconciles exactly: 1,633 - 1,617 = 16 fixtures left the native
bucket, of which **13** merely moved to the (correct) fallback bucket and **3**
are the new genuine failures below. 1,647 - 1,644 = 3.

### 20.2 The 13 extra fallbacks are Apple SDK headers, and they are cosmetic

The fallback bucket splits cleanly by phase:

- **14 link failures** -- `import of undefined item __atomic_fetch_sub` and
  friends. Same `__atomic_*` class as Linux's 13; c2mir has no GCC atomic
  builtins and these come from user/stdlib inline C the emitter does not own.
- **13 compile failures, macOS-only** -- c2mir cannot parse Apple's SDK headers
  when user inline C includes them. Three distinct causes:
  - `TargetConditionals.h:398: #error TargetConditionals.h: unknown compiler`
  - `libkern/OSByteOrder.h:314: #error Unknown endianess.`
  - `dirent.h:77: syntax error on struct` / `dirent.h:109: syntax error on *`
    (Apple's `_Nullable`/`_Nonnull` nullability qualifiers)

  The first two are `#error`s gated on `__clang__`/`__GNUC__`, which c2mir
  defines neither of. This is 9.3's "Apple SDK residue," now measured on the
  product path at 13 fixtures.

All 27 **fall back cleanly and produce correct output**. This is a latency and
coverage cost, not a correctness one, and it is plausibly cheap to shrink: a
small set of predefines (a `__clang__`-shaped identity plus empty
`_Nullable`/`_Nonnull`) would likely recover most of the 13. That is the same
shape as the spike's `subset-shim.h` and should be weighed as J2/J3 work.

One fallback worth calling out as *not* an emitter defect: `thread-local-basic`
fails on `<tur-jit>:7555: syntax error on static`, which is the fixture's own
inline-C `static __thread int64_t tls_val`. Section 16's claim that the emitted
C is c2mir-clean as emitted survives -- every compile-phase fallback is user
inline C or an SDK header, never generated code.

### 20.3 Three genuine failures, all pre-existing Turmeric defects

Both are filed; neither is a MIR defect.

**(a) `map-multiword-struct-key` and `set-multiword-struct-element` -- SIGBUS.**
One shared cause: the emitted TU calls `tur_hamt_hash_xxh64` with **no prototype
in scope**. The preamble declares its siblings `tur_hamt_box_key` and
`tur_hamt_box_key_eq` and simply omits this one. Under Apple's arm64 ABI
anonymous variadic arguments go on the stack, and c2mir treats a no-prototype
call as all-anonymous-variadic, so `&p` and `16` land on the stack while xxh64
reads junk from `x0`/`x1`:

```
thread #2, EXC_BAD_ACCESS (code=1, address=0x101968000)
frame #0: tur`tur_hamt_hash_xxh64 + 104   ->  ldp x17, x2, [x0]
x0 = 0x000000010015d6f8  tur`tur_hamt_hash_xxh64   <- the callee's OWN address, as `data`
x1 = 0x0000000173e06e90                            <- the real &p, as `len`
```

x86-64 SysV passes unprototyped arguments in the same registers either way,
which is why Linux never saw it. Splicing one `extern` declaration into the
fixture's own emitted TU fixes it end to end.

`src/main.c:4913-4917` already names this exact symbol as a known-open concern
and passes `-Wno-error=implicit-function-declaration` to keep the `cc` build
quiet about it. That suppression is hiding a wrong-code bug on a second backend:
even without a struct the JIT computes a *different hash*, and the implicit
`int` return has been truncating this hash to 32 bits on the `cc` path on every
host all along. Filed:
[../archive/jit-xxh64-missing-prototype.md](../archive/jit-xxh64-missing-prototype.md).

**(b) `taskgroup-async` -- rc=0, empty stdout.** A silent wrong answer, worse
than the crash. `emit_module.c` emits **two different, mutually inconsistent**
local `typedef`s for `TaskGroupBlock` and **neither matches** what
`stdlib/taskgroup.tur:78` actually allocates (`pthread_mutex_t lock` first).
`emit_module.c:8497` declares `{bool cancelled;}` and so reads byte 0 of the
initialized mutex -- `0x5A` on this box -- as `cancelled`. Every fiber spawned
into a TaskGroup is therefore marked cancelled before its entry runs; the
futures stay pending, the await lowering parks a continuation nothing resumes,
and `main` never reaches its `println`s.

Reading a byte that is neither 0 nor 1 through a `bool` lvalue is UB and the two
front ends disagree: clang masks to bit 0 (`0x5A & 1 == 0`, so it survives),
c2mir tests the whole byte. On Linux glibc leaves that byte `0`, so the layout
bug is invisible there under *either* front end -- which is the entire reason
the Linux baseline is clean.

The sibling shim at `emit_module.c:8448` is worse and protected by no luck at
all: it writes `cancelled`/`done` over the mutex's first two bytes and calls
`pthread_mutex_lock` on **offset 16**, the middle of the real mutex. That is
corruption on every platform; it is just on a rarely-exercised panic path.
Filed:
[../archive/emitted-taskgroupblock-layout-mismatch.md](../archive/emitted-taskgroupblock-layout-mismatch.md).

Both defects are arguments *for* plan item **S2**: each exists because the
inline-C-facing runtime surface has no single declared boundary, so a symbol
goes undeclared in one place and a struct gets retyped by hand in three.

`any-cast-mismatch-panic` (signal-6, panics by design) and
`hamt-lowering-basic` (the filed `^persistent` key bug) reproduce exactly as on
Linux and are not new.

### 20.4 S2 replays 8/8 -- but the latency case is much weaker on Apple Silicon

`tools/jit-spike/s2-proof-run.sh` (added here; section 19's Linux driver was
ad hoc and never committed) replays 19.2 end to end. All eight subsystem
fixtures pass with the runtime host-resident: `arith`, `hamt-basic`,
`cps-backend-effect`, `stm-stress`, `dynvar-nested`, `module-defer-basic`,
`gc-registry-growth`, `self-recursive-carrier-struct-return`. The runtime half
is 3,521 lines; program halves are 5,373-5,906.

19.3's three seams all reproduce, and macOS reaches seam 3 by a different route:
`ld64` has no `-Bsymbolic`, but its default two-level namespace already
self-binds the dylib's intra-library calls, which is the property `-Bsymbolic`
buys on ELF.

Latency, best of 5, `arith`, eager:

| | c2mir | link+gen | total |
|---|---|---|---|
| full TU (status quo) | 161.7 ms | 60.9 ms | 222.6 ms |
| **split (program half only)** | **153.5 ms** | **32.0 ms** | **185.5 ms** |

**17% of engine time gone, against 38% on Linux.** The split does what it did on
Linux to the half it targets -- link+gen nearly halves (60.9 -> 32.0, 47%;
Linux 141.0 -> 66.6, 53%) -- but on Apple Silicon that half is no longer where
the time is. c2mir dominates at 73% of the total and the split barely touches it
(-5%), because the declarations still parse and, per 8.3, ~36 ms of macOS c2mir
time is the Apple SDK headers alone, which both halves parse identically.

This qualifies 19.2's headline number and sharpens 8.3's conclusion rather than
overturning it. S2 remains worth doing and remains a J2 prerequisite. But on
Apple Silicon **S2 alone does not make the JIT beat `cc`** -- the next lever
after S2 is c2mir's front-end cost, i.e. not re-parsing system headers per
program (a precompiled/serialized header, or keeping system headers out of the
program half entirely). Whoever picks up J2 on macOS should size that before
assuming S2 closes the gap.

### 20.5 Two harness bugs found and fixed

- **`sweep-turjit.sh` classified all 31 `httpd-*` fixtures as `fallback-fail`.**
  Its environmental check matched only GNU ld's `cannot find -lturi`; ld64 says
  `library 'turi' not found`. A purely environmental limit read as 31 JIT
  defects. Now matches both.
- **`tur-jit-spike.c`'s `import_resolver` consulted `TUR_JIT_PRELIB` *before*
  its own intercepts**, contradicting the comment directly above it
  ("Intercepts first"). Latent on Linux because glibc never exports `atexit`
  (9.4), live on macOS because libSystem's `atexit` resolves through the
  dylib's dependency chain -- so the prelib preempted the interception and
  `module-defer-basic` took SIGSEGV at exit under the S2 proof while passing
  under the real `tur jit`. Intercepts now precede the prelib lookup, and the
  proof goes 7/8 -> 8/8.

Both are proof/harness-scale bugs, but the second is a real warning for
production S2: once the runtime library is a genuine host-resident artifact,
resolver **priority** is load-bearing and the ordering has to be stated
deliberately rather than inherited from whichever platform's dynamic linker
happens to hide the conflict.

### 20.6 Reproducing

```sh
cmake -S . -B build-turjit -DCMAKE_BUILD_TYPE=Release -DTUR_JIT=ON
cmake --build build-turjit -j --target tur
bash tools/jit-spike/sweep-turjit.sh          # 1617 native + 27 fallback-pass / 1680

cmake -S . -B build-jit-spike -DCMAKE_BUILD_TYPE=Release -DTUR_JIT_SPIKE=ON
cmake --build build-jit-spike -j --target tur-jit-spike
bash tools/jit-spike/s2-proof-run.sh          # 8 passed, 0 failed + latency table
```

### 20.7 Both macOS-found emitter defects fixed on Linux, same day

Both of 20.3's reports are resolved and archived:

- **xxh64 prototype**: the preamble now declares
  `uint64_t tur_hamt_hash_xxh64(const void *data, size_t len);` -- hamt.h's
  exact spelling, emitted after the standard includes. The first attempt sat
  with the pre-include macro block and broke every fixture on an undeclared
  `uint64_t` -- findings 14.1's mistake repeated verbatim and re-caught by the
  suite within one run, which is an argument for the suite and an indictment
  of the author in equal measure. Note the cc-path side effect: the hash is
  now the full 64 bits everywhere (it had been silently truncated through the
  implicit `int` since the fixture existed); no fixture output depends on
  hash-order, so the suite stayed 2399/0.
- **TaskGroupBlock**: all THREE emitted typedefs (20.3 found two; a third in
  `tur_task_group_notify_done` had correct offsets under a misleading
  trailing field name) now spell the canonical layout verbatim from
  `stdlib/taskgroup.tur:78`.

Verification on Linux: `map-multiword-struct-key`,
`set-multiword-struct-element`, and `taskgroup-async` all pass under the
Linux `tur jit`; suite 2399/0 with 140 snapshots regenerated; the product
sweep is **byte-identical** (1,633 + 14, zero fixture diffs) -- which is the
expected shape for fixing defects Linux was surviving by luck. The arm64
SIGBUS and the silent-empty-stdout repros await confirmation on the next
macOS run; expected result there is native PASS for all three and
**1,647/1,680 matching Linux exactly**.

## 21. macOS confirms both fixes -- full parity -- and the suppression stays

Added 2026-07-29, same Apple M2 / Darwin 27.0.0 / Apple clang 21.0.0 host as
section 20, `tur` rebuilt at `158397346`.

### 21.1 20.7's predicted result, confirmed exactly

All three fixtures are native PASS under `tur jit` on arm64 macOS, and correct
under `tur run`: `map-multiword-struct-key`, `set-multiword-struct-element`
(the SIGBUS pair) and `taskgroup-async` (the silent-empty-stdout one).

`sweep-turjit.sh`, full corpus. Measured twice: first at the fix commit on the
1,680-fixture corpus, then re-measured at `3b2cf9e75` after main's numeric tower
brought the corpus to 1,701 and the JIT prelude gained five math builtins.

| | corpus | native PASS | fallback-pass | **correct** |
|---|---|---|---|---|
| macOS, section 20 (pre-fix) | 1,680 | 1,617 | 27 | 1,644 |
| macOS, at the fix | 1,680 | 1,620 | 27 | **1,647** |
| Linux, at the fix | 1,680 | 1,633 | 14 | 1,647 |
| **macOS, at `3b2cf9e75`** | 1,701 | 1,641 | 27 | **1,668** |
| Linux, at `3b2cf9e75` | 1,701 | 1,654 | 14 | 1,668 |

**macOS matches Linux exactly on both corpora** -- 1,647/1,680 at the fix, and
1,668/1,701 after the numeric tower -- which is what 20.7 predicted. The
remaining composition is identical too: 31 `fallback-env`, one
`output-mismatch` (`hamt-lowering-basic`, the filed `^persistent` key bug) and
one `FAIL` (`any-cast-mismatch-panic`, panics by design).

The native/fallback split still differs by 13 in Linux's favor. That is 20.2's
Apple SDK header residue, unrelated to these fixes, and it is the one macOS
number that has not moved -- the standing item for J2/J3.

### 21.2 NEGATIVE RESULT: `-Wno-error=implicit-function-declaration` is load-bearing

20.3 argued the suppression at `src/main.c` "should be revisited," on the theory
that it was hiding a wrong-code bug and nothing else. **That was tested here and
is wrong.** Recording it so nobody re-derives the same bad idea.

The evidence that looked convincing: emit the C for all **1,934** fixtures and
run `clang -fsyntax-only -Werror=implicit-function-declaration` over each. Zero
implicit declarations. On that basis the flag was removed -- from all **three**
cc invocation sites, not the one the comment describes -- and the whole suite
re-run: **2342 passed / 57 failed, byte-identical** to the run before it, with
zero non-environmental failures.

Both signals were false, for the same reason: **they measure the wrong
artifact.** `tur emit-c` output is not what `tur build` hands to `cc`, and the
57 suite failures already included every fixture that would have exposed the
difference. The JIT sweep caught it, because its step-6 fallback exercises the
real build path:

```
tests_fixtures_httpd-async-echo__input_tur.c:9:119: error: call to undeclared
library function 'malloc' ... ISO C99 and later do not support implicit
function declarations
```

Reading that generated file explains it -- the `#include <stdlib.h>` is on line
**10**, and hoisted user inline C calls `malloc` on line **9**:

```c
  9  static char *httpd_conn_own_cstr(HttpdConn *c, char *s) { ... malloc(sizeof(HttpdOwnedStr)) ... }
 10  #include <stdlib.h>
```

So the suppression is not vestigial: it papers over an **include-ordering defect
in the inline-C hoisting path**, where a `#tur-include`-hoisted block can land
ahead of the standard-header include a later block contributes. Removing the
flag turns that into a hard error for real spice code (the httpd spice here),
which is a user-facing break well outside the scope of the xxh64 fix. The
removal was **reverted** at this point. (It was re-landed shortly after, once
the ordering defect itself was fixed -- see 21.3.)

The ordering defect is the thing actually worth fixing, and it is independent of
the JIT: hoisted includes should precede hoisted code. Until then the comment at
`src/main.c` should say *this* -- an include-ordering workaround -- rather than
naming a single xxh64 call site that is now fixed, because the current wording
invites exactly the removal attempted here. Filed:
[../archive/history/hoisted-inline-c-precedes-includes.md](../archive/history/hoisted-inline-c-precedes-includes.md).

Method note, generalizing past this instance: a corpus-wide static sweep is only
as good as its artifact, and "the suite did not change" is not evidence when the
fixtures that would show the change were already failing for another reason. The
JIT sweep found this precisely because it is the one harness that drives the
real end-to-end path.

### 21.3 The ordering defect fixed, and the suppression retired for real

21.2 ended with the suppression restored and the include-ordering defect filed
but unfixed. Both are now closed.

**The defect was one level deeper than 21.2 described.** It is not "a hoisted
block precedes a plain inline-C include" -- both halves are `__tur_include__`
payloads. That marker carries two different kinds of thing:

```
stdlib/httpd.tur:93    /* __tur_include__: static char *httpd_conn_own_cstr(...) { ... malloc(...) ... } */
stdlib/httpd.tur:3916  /* __tur_include__: #include <stdlib.h> */
```

`hoist_tur_include_directives()` concatenated every payload in **source order**,
so the line-93 code landed ahead of the line-3916 directive. The marker's name
says "include" but roughly half its uses in the corpus are file-scope code
(`typedef` and `static` helpers that must sit above the functions using them),
and nothing kept the two kinds ordered relative to each other.

**Fix:** two buckets. Each payload's first non-whitespace token decides --
`#include`/`#define`/`#undef`/`#pragma` go to a directives buffer, everything
else to a code buffer -- and directives are emitted first. Relative order within
each bucket is preserved, so a feature-test `#define` still precedes the include
it conditions; only the two kinds separate. The corpus uses exactly three
payload shapes (`typedef`, `static`, `#include`), so no conditional-compilation
structure risks being split. `<stdlib.h>` now sits on line 3 of the httpd TU,
ahead of the `malloc` call on line 10.

**No snapshot churn**: `emit-c` does not hoist (the hoist is a post-pass on the
build/JIT paths), so all `expected.c` stay byte-identical -- which is also the
mechanical reason 21.2's `emit-c` sweep could never have detected the problem.

With ordering fixed, `-Wno-error=implicit-function-declaration` is gone from all
three cc invocation sites.

Verification, run the way 21.2 concluded it must be:

| | with flag | flag removed |
|---|---|---|
| `sweep-turjit.sh` | 1,641 native + 27 fallback = 1,668/1,701 | **identical** |
| 31 `httpd-*` | `fallback-env` | **`fallback-env`** |
| `tests/run.sh` | 2373 passed / 57 failed | **identical** |

All 57 suite failures remain environmental (32 `httpd-*` + 15 `reactor-*` on
`-lturi`, 10 `refine-*` on a Release `tur`). The `httpd-*` fixtures compile
cleanly now and fail only at link, where they failed before this whole thread
started.

Report archived:
[../archive/history/hoisted-inline-c-precedes-includes.md](../archive/history/hoisted-inline-c-precedes-includes.md).

## 22. The `^persistent` cstr-key bug fixed -- the sweep's last output-mismatch retired

Added 2026-07-30, after the rebase onto main (post-rebase baseline: 1,654
native + 14 fallback-pass = 1,668/1,701, suite 2430/0).

Section 11.7 filed `hamt-lowering-basic`'s wrong output under MIR as a
product bug, not a MIR defect: the P3 `^persistent` lowering routed cstr
keys through `hamt/hash-ptr` + the identity-comparing HAMT entry points, so
string keys were identity-HASHED and identity-COMPARED. That only ever
"worked" because gcc/clang merge identical string literals in a TU -- which
C11 6.4.5p7 leaves unspecified and c2mir does not do. Any key built at
runtime (concatenation, parsed input) was silently lost on every path, JIT
or not.

The fix follows the report's first direction, one layer lower than proposed:

- **Runtime** (`src/runtime/hamt.c/.h`): content-keyed cstr entry points
  `tur_hamt_set_cstr` / `del` / `has` / `get` -- one call that content-hashes
  (`tur_hamt_hash_str`) and content-compares on collision (an internal
  strcmp comparator through the TCE4 `_eq` family, the same semantics as the
  GHE path's `MapKey[cstr]` comparator).
- **stdlib** (`stdlib/hamt.tur`): `hamt/set-cstr` / `hamt/del-cstr` /
  `hamt/get-cstr` / `hamt/has-cstr?` -- plain-Turmeric wrappers, so the
  interpreter evaluates them without an inline-C carve-out.
- **Elaborator** (`elab_call.c`): each P3 arm (assoc/dissoc/get/has?) checks
  the elaborated key's type; `TY_CSTR` routes to the cstr wrapper (the arm's
  args are already the wrapper's exact signature -- no hash-call synthesis).
  Non-cstr keys keep hash-ptr identity semantics, and mixing key kinds in
  one map stays sound: the comparator is per-operation and consulted only on
  a 64-bit hash collision.
- **Interpreter** (`src/turi/collections_native.c`): four natives backing
  the new extern-c leaves, with the same retain-on-no-change quirk as
  `native_tur_hamt_set`.

`hamt-lowering-basic` now probes and deletes through **runtime-built keys**
(`str-concat`: equal text, distinct pointer) alongside literals, per the
report -- the fixture can no longer pass by literal merging. It is
whitelisted in `run-turi.sh`'s `TURI_INLINEC_RUN` (verified to interpret
correctly; the inline-C it loads is str-build.tur, whose `str-concat` has a
native override).

Verification: the report's cc-path repro prints true/true (was
false/false); suite **2430/0** with all 140 snapshots regenerated (the new
stdlib defns shift gensym counters in every TU); turi suite 1,680/1
(the one failure, `hkt-constrained-byvalue-bind-pure`, is a pre-existing
interpreter HKT gap -- A/B-confirmed against the pre-change tree and filed
in `docs/reported/`); product sweep **1,655 native + 14 fallback-pass =
1,669/1,701**, output-mismatch bucket now **empty**. The only remaining
non-environmental line in the sweep is the by-design
`any-cast-mismatch-panic` signal.

A consistency bonus: `#map{...}` data literals already content-hash their
string keys (`elab_toplevel.c` lowers keyword/string keys through
`hamt/hash-str`), so a `^persistent`-bound literal map probed via `has?`
used to mix content-hashed inserts with pointer-hashed probes -- misses on
every C compiler, literal merging or not. The P3 arms now agree with the
literal path.

## 23. S2 production, step 1 -- the split artifacts exist, generated and committed

Added 2026-07-30. This lands 19.4 item 1 (the artifacts + generator + hash)
and the validation harness for them; items 2 (runtime TU into `tur`) and 3
(cmd_jit consumption) remain.

### 23.1 The all-gates emission mode and `tur emit-rt-split`

`emit_rt_split_source()` (emit_module.c) emits the feature-complete
SINGLE-FILE runtime preamble: every program-gated block forced on via a new
`g_rt_split_all_gates` flag OR'd into the six `cps_uses_*` scan gates --
the same effect the `shared ||` alternatives give `--shared` mode, extended
to the program-scan gates -- plus the five boolean gate globals
(hamt/regex/variadics/winsock/cps_path) saved, forced, and restored.
Normal emission is byte-identical (flag is false; suite 2430/0, snapshots
untouched).

Two consumers see this text, and they must agree byte-for-byte:
`tur emit-rt-split` (the generation input; `--hash` prints its xxHash64 via
`tur_hamt_hash_xxh64`, so both sides share one hash spelling) and, next
step, cmd_jit's probe. Knobs are deliberately NOT normalized in the mode:
`--backtrack-depth` interpolates into a `#define`, `--enable`
experiments swap emitted bodies, archive mode flips rc/GC between
definitions and prototypes -- all of it lands in the text, so any knob
drift fails the hash compare and falls back. The subcommand hard-sets
archive mode (the committed artifacts describe the JIT-time state, where
DEDUP-4b resolves rc/GC from the archive); a JIT invocation with no
archive hashes differently and self-excludes. 3,295 lines, deterministic.

### 23.2 The generator and the committed artifacts

`tools/gen-runtime-split.py` is the validated proof transform
(s2-split-proof.py) promoted to a generator writing
`src/runtime/generated/`:

- `tur_rt_split.c` (3,305 lines) -- the runtime TU: statics externalized
  (`static inline` loses both), the 11 thread-locals routed through the
  host `tur_tls_*` accessors. Compiles clean under gcc first try.
- `tur_rt_split_decls.h` (1,324 lines -- 60% below the preamble's 3,295) --
  the declarations region cmd_jit will splice ahead of the program half.
- `tur_rt_split_embed.c` -- the decls as a C string plus
  `tur_rt_split_hash`, to be linked into `tur` so the JIT path needs no
  install-path lookup.

One correctness addition over the proof: a `static` prototype whose
definition is NOT in the preamble is per-program (defined below the marker
-- `__tur_static_init` is the case, detected by a definitions census). The
proof blanket-de-static'd it, declaring `void f(void);` and then compiling
the program half's `static void f(void){...}` after it -- c2mir tolerates
that linkage conflict, gcc would not have. The generator keeps such
prototypes verbatim in the decls half and drops them from the runtime TU.

### 23.3 Validated: 8/8 subsystems and a 300-fixture sample, zero failures

`tools/jit-spike/s2-artifacts-run.sh` validates the COMMITTED artifacts --
distinct from s2-proof-run.sh's per-fixture re-splitting: ONE runtime .so
built from the committed all-gates TU, and each fixture's emitted TU cut at
the marker with the COMMITTED union decls spliced ahead of the program
half. This is also the superset-property test 19.4 needed: a program half
emitted under ITS gates must bind against the union runtime.

All 8 subsystem fixtures pass, and a 300-fixture random sample from the
product sweep's PASS bucket passes 300/300. Latency on `arith`, best of 5
(union artifacts, not per-fixture): full TU 87.4 + 142.0 = 229.4 ms; split
74.1 + 65.1 = 139.2 ms -- **39% off engine time**, matching 19.2's 38%
within noise. The c2mir cut is smaller than the per-fixture proof's (the
union decls are bigger than arith's own preamble) while link+gen halves
(the runtime's 198 functions are not re-generated per program).

Two runner potholes worth recording: the union decls carry
`#include "hamt.h"` even for programs that never gated it in, so the
consumer needs `-I src/runtime` (cmd_jit already passes include dirs); and
the first validation run failed 8/8 on `unresolved import:
tur_hamt_set_cstr` -- a stale spike binary predating section 22's runtime
addition, not an artifact defect. Rebuild first; the harness binary must be
at least as new as the compiler that emitted the program half.

### 23.4 What remains for production S2

1. Compile `tur_rt_split.c` into `tur` itself (and `libturi.a`), REPLACING
   the host's diverged `cps_rt.c`/`stm.c`/fiber duplicates -- seam 3's
   "the runtime library must BE the runtime". The big, risky step: those
   TUs also serve turi natives and the REPL host paths today.
2. cmd_jit: hash-probe `emit_rt_split_source` against the embedded
   `tur_rt_split_hash`; on match emit decls + program half; on mismatch
   fall back to the full preamble (status quo).
3. Then re-run the product sweep expecting the same 1,669/1,701 at lower
   engine latency, and regenerate-artifacts becomes part of the
   preamble-change workflow (same-PR policy as fixture snapshots).

## 24. S2 production, step 2 -- the runtime library IS the runtime

Added 2026-07-30. This lands 19.4 item 2: the generated runtime TU is now
compiled into `tur` (via tur_core, hence also libturi.a), REPLACING the
host's diverged copies. Only cmd_jit consumption (item 3) remains.

### 24.1 The five diverged TUs, measured before removal

`nm` over every libturi member against the generated TU's 272 external
symbols: the overlap is **58 names across exactly five TUs** --
`runtime/runtime.c` (panic/catch-unwind/cloneable-cont), `runtime/
cps_prompt.c` (the DK machine), `runtime/stm.c`, `async/scheduler.c`,
`async/timer_wheel.c`. Three facts made the swap safe, each verified
rather than assumed:

- **No host consumer**: zero undefined references into those five from any
  other libturi member, and zero from src/turi, main.c, or the REPL. They
  were pure dlsym-export surface -- the dormant wrong-vintage exports that
  seam 3 warned would preempt a split program's runtime calls.
- **No unit-test loss**: tur_cps_prompt_unit and tur_cps_rt_unit compile
  their subject .c files directly into the test binary (and cps_rt.c, the
  file findings 19.3 loosely named, has ZERO overlap -- the tur_kont_*
  trampoline family stays untouched in tur_core).
- **No archive-mode loss**: TURT_RUNTIME_SOURCES (libturt_runtime.a,
  --runtime=lib) never contained the five.

The five .c files stay in the tree as unit-test subjects and vintage
reference; they are simply no longer part of tur/libturi.

### 24.2 The first sweep after the swap failed 1,656 fixtures -- one symbol

`tur_rt_split.c` compiled and linked cleanly (after per-file warning relax
to TUR_CC_FLAGS' bar -- it is emitted C, and tur_core's pedantic -Werror
profile rejects fn-ptr/object-ptr casts every compiled program makes), the
full suite stayed 2430/0, unit tests green... and the product sweep
collapsed to **0 jit-native**: `import of undefined item
tur_set_contract_handler` on 1,656 fixtures.

The miss in 24.1's method: the nm cross-reference covered HOST consumers,
but the emitted programs themselves resolve host symbols by address, and
runtime.c carried one API pair that is NOT part of the preamble -- the CT4
contract-handler registry (stdlib/contract.tur inline-C calls
tur_set/get_contract_handler extern). Now extracted to
`src/runtime/contract_handler.c`, kept in tur_core.

Why the cc path never noticed the same hole (suite was 2430/0 the whole
time): the referencing stdlib functions are emitted `static`, gcc discards
them when unused, and the undefined reference vanishes with them. c2mir
does no unused-static elimination, so the JIT surfaces the import in every
stdlib-bearing program -- the JIT as ABI-canary again, this time for the
host-residency surface itself.

### 24.3 End state, fully validated

With contract_handler.c restored: product sweep **1,655 native + 14
fallback-pass = 1,669/1,701 -- byte-identical composition to the pre-swap
sweep** -- on a `tur` whose exported dk_prompt/tur_atomically/
tur_stm_*/scheduler/timer-wheel families are now the CURRENT emitted
vintage, generated from the same preamble programs compile against. Suite
2430/0, turi suite 1,680/1 (same pre-existing HKT gap), unit tests green.

Seam 3 is thereby closed at the root: there is no second vintage left in
the process for a split program half to mis-bind against. cmd_jit's
hash-gated decls+program emission (item 3) can now land.

## 25. S2 COMPLETE -- cmd_jit runs the split, the sweep is byte-identical

Added 2026-07-30. 19.4 item 3, the last piece: `tur jit` now swaps the
fixed preamble for the committed declarations region whenever the compiler
still matches the artifacts.

### 25.1 The wiring

After compile_to_c + hoist, cmd_jit re-emits the all-gates preamble under
current process state (emit_rt_split_source -- AFTER program emission, its
registry resets must not disturb it), hashes it, and compares against the
embedded `tur_rt_split_hash` (src/runtime/generated/tur_rt_split_embed.c,
linked into TUR_JIT builds only). On a match it splices
[hoisted user prefix][committed decls][program half] -- the hoisted prefix
must survive, findings 21.3's two-bucket block sits ABOVE the preamble --
and hands that to the engine. `TUR_JIT_NO_SPLIT=1` opts out.

Failure ladder: a split-path COMPILE/LINK failure prints TUR-W0071 and
retries the full TU in the engine before conceding TUR-W0070 to cc -- the
hash guard covers emitter drift, not e.g. an export a host build dropped,
and the full TU is self-contained against that. RUN failures are the
program's own and are not retried.

### 25.2 Validation

Product sweep with the split live: **1,655 native + 14 fallback-pass =
1,669/1,701, results.tsv byte-identical** to the pre-split sweep. The 14
TUR-W0071 retries are exactly the user-inline-C `__atomic_*` fixtures
(split half fails c2mir, full TU fails identically, cc passes) -- no
fixture needed the retry to SUCCEED, i.e. the split path has no holes the
full path was papering over. 45 TUR-W0070 = 31 fallback-env + 14
fallback-pass, all accounted for. Suite 2430/0 on the non-JIT build
(the splice code is TUR_HAVE_JIT-guarded; non-JIT builds skip the ~100KB
embed TU entirely).

Latency, arith, wall-clock including the compiler front end, 5 runs:
~200ms split vs ~278ms full -- the 39% engine-only cut of 23.3 lands as
~28% end-to-end.

### 25.3 The regeneration workflow

When the preamble changes: `python3 tools/gen-runtime-split.py --tur
./build/tur` and commit the three artifacts in the same PR (fixture-
snapshot policy). Forgetting is safe -- the hash mismatch quietly reverts
every `tur jit` to full-preamble emission until the artifacts catch up;
`tools/jit-spike/s2-artifacts-run.sh` is the standalone artifact check.

S2 is thereby COMPLETE: the runtime boundary is named (the marker), the
runtime library IS the runtime (section 24), and the JIT compiles each
program against declarations, resolving the runtime by address into the
host process. J2 (REPL integration) is unblocked.

## 26. J2 LANDED -- the REPL builds spices in process

Added 2026-07-30. Plan section 3.3: `tur --enable=jit repl` swaps the
`tur build --shared` subprocess + dlopen + dlsym pipeline for the MIR
engine, in process. Cold spice load drops **~850ms -> ~260ms (3.2x)** on
the two-module probe spice -- the subprocess + cc + link round trip
replaced by one c2mir pass over the S2 split (the decls region + program
half; a REPL reload is exactly the loop S2 exists for).

### 26.1 The shape

- **Engine** (jit_engine.c): the one-shot front half is factored into
  jit_compile_and_link (prelude, c2mir, module load, eager MIR_link,
  config-global sync), shared by tur_jit_execute and the new persistent
  TurJitImage API: compile_image / image_sym / image_free.  image_sym is
  MIR item lookup by name -- which, unlike dlsym, sees `static`
  functions, so the single-TU spice emission needs no linkage changes.
  compile_image explicitly calls `__tur_static_init` (S1b's no-main
  case: c2mir discards the constructor attribute).  image_free drains
  the engine's atexit list (module defers) while the code is still
  mapped; callers rebind before freeing (the (reload) order).
- **Loader** (spice_loader.c): a TurSpiceJitHook (build / sym /
  free_image) installed by main.c at REPL start when the `jit`
  experiment is on -- the loader is tur_core, the engine is
  TUR_JIT-only, so a function-pointer table is the seam.  With the hook
  set, tur_spice_image_load skips needs_rebuild/run_build/dlopen
  entirely; the manifest arrives as in-memory text; freshness for
  (reload)/--watch is source mtime vs the image's build stamp.
- **Build glue** (main.c repl_jit_build): the whole spice compiles as
  ONE single-file TU -- a synthetic root module importing every source
  module, so imports dedupe through the ordinary module machinery.  Two
  probe-driven discoveries shaped this: a LOAD-based root duplicates any
  module also imported intra-spice (sh__add42 defined twice), and module
  resolution is FILENAME-based while --shared-path spices may name
  modules freely (defmodule oth in other.tur) -- so a SHADOW DIR of
  module-name -> file symlinks under .tur-repl-cache/jit-mods/ makes
  every module importable uniformly.  The manifest comes from the same
  compile (a g_manifest_sink capture around emit); the S2 hash-gated
  preamble swap and the W0071 full-TU retry ladder are the same helpers
  cmd_jit now uses (jit_try_split_preamble / jit_sdk_include_dirs).

### 26.2 Two real defects the tests caught

- **Interpreter-mode leak.** The REPL process runs with
  g_interpret_mode=true (turi_env_new), and the in-process compile
  inherited it: unknown names demoted from hard errors to W0040
  runtime-dispatch warnings -- and, unexercised but worse, `#?(:turi
  ...)` branches would have been selected into NATIVE code.  The
  subprocess never saw the flag because it was a fresh process.  Cleared
  for exactly the compile.  This is the class of bug in-process
  integration keeps buying: process-global state the subprocess boundary
  used to launder (g_emit_for_link got the same save/restore).
- **Missing __ffi shims.** interpreter-arbitrary-arity-ffi's per-export
  shims were emitted only on the --shared path; the three high-arity RP4
  tests failed under the image build.  emit_program now emits them
  behind g_emit_ffi_export_shims, set only by repl_jit_build -- every
  other emission stays byte-identical (suite 2430/0, snapshots
  untouched).

### 26.3 Validation

The four RP suites driven through `--enable=jit`: **call 14/14, reload
4/4, watch 5/5, errors 3/5** -- the two failures are structurally
subprocess-specific (a manufactured stale exports.manifest FILE, which
the in-process path cannot have, and a strings(1) grep over the $TUR
wrapper script rather than the binary).  The same suites on the
subprocess path (hook not installed): all green, byte-for-byte the
loader refactor is inert when off.  New tests/turi/repl-spice-jit.sh
(ctest tur_repl_spice_jit, TUR_JIT-gated, self-skipping) pins the
in-process path: load with no .so artifact, bare/qualified/float
(fractional probe)/arity-12 calls, the compile-error surface, (reload)
self-heal.  Suite 2430/0; product sweep unchanged at 1,655 + 14 =
1,669/1,701 after the engine refactor (the moved free of the source
buffer validated under ASan by the whole sweep).

### 26.4 Recorded v1 limits

Transitive :spices deps are not auto-appended to the image build's
include path (single-spice projects only; the subprocess path remains
the default and handles them), and POSIX symlinks gate the shadow dir
out of Windows along with the engine itself.  Both are hook-local:
lifting them touches repl_jit_build only.

## 27. J3 LANDED -- the parity harness and the triangle, plus two latent miscompiles

Added 2026-07-30. Plan section 5's J3: the fixture corpus runs under
`tur jit` through a first-class harness, and the engine triangle is
measured and published.

### 27.1 tests/run-jit.sh

Modeled on run-turi.sh (scan, markers, stamp cache, parallel fan-out,
errors/ diag pass) with run.sh's RESULT semantics (expected.stdout diff +
expected.exit + run.args + input.stdin) -- which is why the by-design
panic fixture that the sweep scores as FAIL passes here: the harness
honors `expected.exit: nonzero`, the sweep scores signals. The engine's
step-6 cc fallback is a first-class outcome, tallied separately (the
harness exports run.sh's `-L` in TUR_CC_FLAGS, so the sweep's 31
"fallback-env" fixtures link and pass here). New marker: `requires.cc`
(phase-separated dump fixtures whose --dump-* output run.sh discards
with the build invocation; under one-process jit it interleaves with
program stdout). hook.sh fixtures stay run.sh-owned.

Result: **Debug build 2,390 passed / 0 failed / 49 skipped (47 via cc
fallback)**; Release differs by exactly the known Debug-only refine-*
discharge set, same posture as run.sh. ctest: `tur_jit_fixture_tests`
(TUR_JIT-gated, RUN_SERIAL, TUR pinned to the configuring build's
binary).

### 27.2 The harness compiles what run.sh never did -- and found two miscompiles

run.sh scans only `tests/fixtures/*/`; this harness (like run-turi.sh)
scans one level deeper -- so it is the FIRST harness to COMPILE the
nested `typed/*` fixtures, and two of them miscompile on every compiling
engine (gcc and MIR agree; the interpreter is correct, which is why the
suite stayed green):

- `typed/result-basic` -- the `__cps` clone assigns a by-value
  `(Result int int)` struct to an int64 carrier; hard cc error
  (FIXED, section 28:
  docs/archive/history/typed-result-map-cps-clone-struct-assign.md).
- `typed-slots/cs3-nested-specialization` -- the nested-specialization
  float slot prints an int bit pattern through a double
  (FIXED, section 30:
  docs/archive/history/typed-slots-nested-specialization-float-garbage.md).

Both were denylisted in the harness with report pointers; both are
fixed (sections 28 and 30) and the denylist is empty. The J0-era lesson generalizes again: every new engine or harness
that runs code a previous one skipped surfaces real, latent product bugs
-- the JIT keeps being the canary, this time just by scanning deeper.

### 27.3 The triangle, and a third find

benchmarks/triangle/ (fib / loop-sum / mandel -- pure Turmeric, no
imports, no inline C, exact int checksums) + benchmarks/run-triangle.sh,
which refuses to time engines that disagree on output. Numbers and
reading are in docs/guides/performance-guide.md ("Execution engines"):
the front end dominates one-shot latency (~200ms of every leg), the
interpreter wins tiny scripts by skipping compilation entirely and loses
9x on loops, `tur jit` matches cc's one-shot wall with its structural
win in-process (J2's reload), and native runtime is 4-7ms -- nothing
else close in steady state.

Writing loop-sum found the third bug of the phase: **named-let
self-recursion is not emitter-TCO'd** -- it survives the cc path solely
because gcc's sibling-call optimization turns the emitted self-call into
a jump; MIR performs no such optimization, so 5M iterations SIGSEGV on
the default 64MB entry stack (TUR_JIT_STACK_MB=2048 passes, confirming
depth). Filed (and FIXED for the capturing form, section 29):
docs/archive/history/named-let-self-tail-not-tco.md. Per the
standing owner decision, the fix direction is extending the defn-level
TCO rewrite to named-let, never a bigger stack.

## 28. First of the three fixed -- the cps->direct aggregate-carrier bridge

Added 2026-07-30, resolving the first of section 27's finds:
`typed/result-basic` (docs/archive/history/typed-result-map-cps-clone-struct-assign.md).

### 28.1 Two representations of one ADT, and a delivery that admitted only one

The fixture's `test-result-map-preserves-tag` is CPS-colored, and its
`__cps` clone tail-calls `result-map`. The CPS IR types that call at the
ERASED `(Result A B)` carrier -- one int64 word, which is how the stdlib
prelude actually represents a Result (`ok`/`err` malloc a
`tur_result_box_t` and hand back the pointer; `ok?` derefs it through
`tur_is_ok`). But the emitter RESOLVES the callee to a monomorph/spec
clone, `result_map__spec__tur_adt_Result__int__int_int64_t_int64_t`,
which returns its ADT **by value** as a C struct.

So the delivery had a struct in hand and a one-word slot to put it in:

```c
int64_t __t0;                                          /* join local */
tur_adt_Result__int__int __t238 = result_map__spec__...(...);
__t0 = __t238;                                         /* <-- ill-typed */
```

gcc: "incompatible types when assigning to type 'int64_t' from type
'tur_adt_Result__int__int'". c2mir: "incompatible types in assignment to
an arithmetic type lvalue". Every compiling engine agreed; only the
interpreter, which never sees this C, ran the fixture.

`emit_deliver_ty` boxes a Tier-C aggregate on the KK_RET / KK_PROMPT
paths (`slot_store_reap`), but the KK_VAR inline-join path is a plain C
assignment with no conversion at all -- and the value's aggregate-ness is
not visible from the IR type anyway. It lives only in the signature side
table (`emit_sig_lookup_ret_ctype`), which knows what the clone was
actually declared to return.

### 28.2 The fix: bridge concrete->carrier the way the direct emitter does

The direct emitter already crosses this exact boundary, by spilling the
aggregate and passing its ADDRESS:

```c
tur_adt_Result__int__int __t225 = __ps_224;
result_hyeq_qu((int64_t)(intptr_t)(&__t225), ...);   /* int64 carrier param */
```

which works because every carrier consumer of an ADT word derefs it --
`tur_is_ok`, and the spec clone's own `(tur_adt_Result *)(intptr_t)r`
parameter cast. The CPS delivery now does the same thing, heap-copied
rather than stack-addressed because a delivery can sit inside a nested
block while the join label is outside, so `&local` would dangle:

```c
__t0 = (int64_t)__dk_reap_ptr((intptr_t)({
    tur_adt_Result__int__int *__bx = malloc(sizeof *__bx); *__bx = __t238; __bx; }));
```

`__dk_reap_ptr` registers the box on the per-run reap list, so it is
freed at the outermost entry boundary -- the fixture is clean under
`ASAN_OPTIONS=detect_leaks=1`.

Mechanically (src/compiler/emit_cps_ir.c):

- `cty_is_byval_agg(cty)` -- keyed on the C SPELLING, not a Type, because
  the sig side table is the source of truth for what a spec clone returns
  and it carries only the string. Positive list (`tur_adt_*`, not a
  pointer) so anything unrecognized stays a word: the historical behavior.
- `deliver_slot_cty(ce, kont)` -- the C type the delivery lands in: the
  join local's declared type (KK_VAR, newly recorded in `ce->joins[]`
  alongside the name), or the crossing type `slot_store_reap` keys its
  own boxing on (KK_RET / KK_PROMPT). Comparing the two is what keeps the
  bridge from double-boxing a slot that is ALREADY the aggregate.
- The CT_TAILCALL cps->direct arm bridges iff the callee's real return
  ctype is an aggregate and the slot is not.

Caveat recorded rather than engineered around: an aggregate with owning
(rc/ref/weak) fields would cross as a bitwise copy whose box is reaped
with a bare `free`, so owned fields are not dropped -- the same caveat
`slot_box_ty`'s Tier-C box carries, except Tier C can decline and fall
back while this site's only alternative is the hard compile error it used
to be. No fixture reaches it.

### 28.3 Blast radius: one fixture

A sweep of `emit-c` over the whole corpus (both scan depths) finds the new
bridge in exactly ONE fixture -- `typed/result-basic` itself. No
`expected.c` snapshot moves, so there is no regen in this change. Suites:
`tests/run.sh` **2430 passed / 0 failed**; `tests/run-jit.sh` on the Debug
JIT build **2391 passed / 0 failed / 48 skipped** (47 via cc fallback) --
one more pass and one fewer skip than section 27.1, which is the fixture
coming off the harness denylist.

All three of the phase's finds are now fixed -- sections 28, 29 and 30.

## 29. Named-let TCO -- one surface syntax, two lowerings, two different bugs

Added 2026-07-30, resolving the second of section 27's finds
(docs/archive/history/named-let-self-tail-not-tco.md) and splitting a second defect
out from under it.

### 29.1 The named let has two lowerings, and neither was a loop

`(let go [...] ... (go ...))` lowers one of two ways depending on whether
the loop body reads anything from the enclosing function:

| shape | lowering | self-call was |
| --- | --- | --- |
| captures an outer var | lifted closure thunk `__fn_N(void *env, i, acc)` | a real recursive call |
| captures nothing | CPS-COLORED `__fn_N__cps(i, acc, DK *)` | a call to its own DK ENTRY WRAPPER |

Both overflow the stack at depth; only the first is a TCO gap. They had
been reported as one bug because they share a surface syntax.

### 29.2 The capturing form: self-TCO rejected every closure

`tco_params_simple` opened with `if (fd->is_variadic || fd->closure) return
false;`, so no lifted closure thunk was ever eligible -- which is exactly
what a capturing named let compiles to. The emitted self-call survived the
cc path only because gcc -O2's sibling-call optimization rewrote it into a
jump; MIR performs no such optimization, so the same program SIGSEGV'd
under `tur jit`. (Relying on an optimization the language never asked for
is not a guarantee, and section 27.3 is where the bill came due.)

The env pointer is what made this look unsafe, and it is exactly what makes
it safe: a closure's own recursive self-call threads the env it was CALLED
with, never a freshly built box. The closure-call emitter already depends
on this -- it detects the self-call and passes the raw env param (the "S5"
arm in emit_expr.c) -- so the env is loop-invariant across a backedge by
construction. The fix reassigns the source params and leaves params[0]
alone:

```c
static int64_t __fn_1331(void * __env_p_1334, int64_t i, int64_t acc) {
    struct __env_1333 *__env___env_1333 = (struct __env_1333 *)__env_p_1334;
    __tur_tailcall:;
    if ((i) >= (__env___env_1333->n)) { return acc; }
    else {
        int64_t __t42 = (i) + (INT64_C(1));
        int64_t __t43 = (acc) + (i);
        i = __t42; acc = __t43;
        goto __tur_tailcall;
    }
}
```

Mechanically (src/compiler/emit_fns.c): `tco_env_offset(fd)` names the skew
(1 for a closure thunk, 0 otherwise); `tco_params_simple` and
`emit_tail_backedge` iterate from it, the backedge reading
`call->args[i - off]`; `tco_is_self_call` compares arity against
`n_params - off` and matches the closure by
`fn_binding->closure_fn_binding == fd->binding` -- the SAME identity test
the closure-call emitter uses to decide it is looking at a self-call, so
the backedge and the ordinary call can never disagree about which env is
live. `tco_param_type` reads `fd->param_types[i]` for a closure rather than
indexing the source fn type, which has no env slot (the pbp scan
side-steps the same skew by skipping closures outright).

Blast radius: 7 fixtures gain a closure backedge -- the
`letrec-self-recursive-*` family (carrier/float/struct/vec returns),
`constrained-instance-closure-element-dispatch`, `refine-macrogen-foreach`,
`w3-letrec-open-capture` -- all passing, none carrying an `expected.c`, so
no snapshot regen. New fixture `tco-named-let-capture-deep` (5M iterations,
`requires.compiled`) pins it.

### 29.3 The non-capturing form is not a TCO bug at all -- and it is worse

With the capturing case fixed, the same program written WITHOUT a capture
still died -- and, unlike the first, it dies on the cc path too. A
non-capturing named let is CPS-colored (it is a lifted lambda, so the
coloring analysis conservatively assumes an indirect call could reach a
control operator), its body is direct-emitted as a value, and its self-call
targets the function's own DIRECT ENTRY WRAPPER -- `__dk_entry_depth++`, a
`dk_prompt` malloc, a `setjmp` -- once per iteration. Nothing for a
sibling-call optimization to elide, and a prompt per iteration besides.

That is a different root cause with a different fix, so it is a separate
report:
docs/archive/history/cps-colored-noncapture-named-let-recurses-through-entry.md
(FIXED, section 31 -- and by call-target resolution, not the eviction this
paragraph reaches for).  There is a settled precedent for the shape of the answer -- the
recursive-await eviction (emit_cps_ir.c:1815, :2250,
docs/archive/cps-async-recursive-await-eviction.md) records the decision
that a self-recursive shape the DIRECT emitter's TCO handles in O(1) stack
must EVICT from the CPS path rather than lower through DK. Applying it here
means extending the eviction gate, which sits inside the S-set fixpoint
whose own comments warn that a mis-marked entry splits a handler from its
performer; not something to change unreviewed at the end of a stretch.

### 29.4 The guide was documenting a guarantee it did not deliver

docs/guides/performance-guide.md listed "the named-let / loop idiom" under
the self-tail-call guarantee -- and its worked example was the NON-capturing
shape, which SIGSEGVs at 5M iterations on every engine. The guide now shows
a capturing loop (verified to complete under both cc and `tur jit`), states
what the capture has to do with it, and lists the non-capturing case under
the boundary list as a known gap rather than a design limit, with the
workaround (read an enclosing parameter in the loop body) and the report
pointer.

Suites after the change: `tests/run.sh` **2431 passed / 0 failed**;
`tests/run-jit.sh` on the Debug JIT build **2392 passed / 0 failed / 48
skipped** (47 via cc fallback) -- the new fixture, passing natively under
MIR, which is the engine that could not fake it.

## 30. The last of the three -- a return-ABI sibling, and the hole it hid in

Added 2026-07-30, resolving the third and final find of section 27
(docs/archive/history/typed-slots-nested-specialization-float-garbage.md) and closing
the coverage gap that let all three sit undisturbed.

### 30.1 Two clones, identical arguments, different return ABI

`typed-slots/cs3-nested-specialization` printed `3.14` then `4.61425e+18`.
The emitted C says it plainly:

```c
static double use_second__spec__double_tur_adt_Pair__int__float(tur_adt_Pair__int__float p) {
        int64_t __ps_166 = (pair_second__spec__int64_t_tur_adt_Pair__int__float(p));
        return __ps_166;                       /* int64 -> double NUMERIC conversion */
}
static int64_t pair_second__spec__int64_t_tur_adt_Pair__int__float(tur_adt_Pair__int__float p) {
        return ((union { double s; int64_t d; }){.s = ((double)(p).snd)}).d;   /* BIT PATTERN */
}
```

The callee is correct: on the carrier ABI it returns the double's bits in an
int64. The caller is correct in isolation too. What is wrong is that a
`double`-returning spec called the `int64_t` SIBLING -- two clones of
`pair-second` with identical argument types that differ only in return ABI --
and then converted rather than reinterpreted. `4.61425e+18` is what 3.14's bit
pattern looks like read as a number.

`pair-second`'s own `double` spec existed the whole time, three lines away.
The call just did not select it.

### 30.2 The recovery already existed, gated one case too narrowly

Tracing the selection showed the wrong clone was chosen at RECORD time, in the
ABI scan (`emit_abi_record_specialized_call`), not at emit time. The inner
call's `call->type` is the elab-collapsed int64 carrier -- `B` is bound only by
the ACTIVE spec, not by anything the call itself carries -- so the interned
spec's result stayed `int64_t`.

emit_module.c already had the fix for this, "M6 / gap G6(c)", whose comment
describes the bug in advance:

> Recover the concrete PRIMITIVE / register-class result from the active
> spec's bindings so the call resolves to the right return-spec
> (`re_cata__spec__double`) instead of a spurious int64 sibling whose return
> register class (rax vs xmm0) is wrong.

It was gated on `is_passed_closure_clone`. `use-second` is an ordinary
function spec, so the recovery never ran and produced precisely the spurious
int64 sibling the comment names. The gate is now dropped.

One correction on the way: removing the gate outright also recovered by-value
AGGREGATE results, which ride the int64 carrier by deliberate convention --
un-collapsing one retyped a spec's return while its consumers still passed the
carrier (`constrained-loop-vec-push-byvalue-result-element` broke with
"incompatible type for argument 2 of vec_hypush_ex"). Outside a closure clone
the recovery is therefore held to what the comment always SAID it recovers: a
register-class primitive. Aggregates keep their own recovery path, the one
keyed on the call's own bindings.

### 30.3 The actual hole: run.sh never scanned group directories

All three of section 27's bugs shared one cause of INVISIBILITY. `tests/run.sh`
iterated `tests/fixtures/*/`, and a group directory (`typed/`, `typed-slots/`,
`recursive-types/`, `lambda-call-head/`, `lang-dispatch/`) is not a fixture --
it holds them. So its children were dispatched by no compiling harness at all;
run-turi.sh scans that deep but only interprets. **52 fixtures had never been
compiled by the default suite.**

run.sh now descends into a group dir. The test is structural and deliberately
strict: a dir is a group only when it holds NOTHING BUT subdirectories AND at
least one of them carries an `input.tur`. Both halves earn their keep -- a
project fixture driven by `build.tur`/`hook.sh` instead of `input.tur`
(`workspace-ls2/`, `spice-resolver-ok/`, `reader-macros-*`) has regular files
and stays a fixture, and one whose only entry is a source dir
(`module-transitive-imports/src/`) passes the first half but fails the second.

A looser first attempt (descend whenever there is no `input.tur`) silently
DROPPED ~34 project fixtures while still reporting `0 failed` -- the exact
invisible-coverage-loss this change exists to end, reproduced inside the fix
for it. It was caught by the arithmetic not working: 52 fixtures added moved
the total by only 18. The dispatch sets are now diffed explicitly -- the new
set drops exactly the 5 container dirs (never runnable) and adds exactly the 52
fixtures -- rather than inferred from a green run.

`tests/run.sh` **2478 passed / 0 failed** (2431 before: +52 fixtures, -5
container dirs). `tests/run-jit.sh` on the Debug JIT build **2393 passed / 0
failed / 47 skipped**, with the harness denylist now empty and kept as the
mechanism for carrying a compile-path miscompile without hiding it.

### 30.4 Closing the phase

All three of J3's finds are fixed (sections 28, 29, 30), and the structural
gap that hid them is closed. The second defect section 29 split out is fixed too (section 31):
docs/archive/history/cps-colored-noncapture-named-let-recurses-through-entry.md.

The J0-era lesson has now paid out four times in this phase: a new engine or
harness that runs code a previous one skipped finds real, latent product bugs.
Worth stating the corollary plainly, since it is the reusable part: the bugs
were not caused by the JIT and were not even engine-specific -- gcc and MIR
agreed on every one of them. They were invisible because nothing compiled that
code. Coverage gaps do not announce themselves; they read as green.

## 31. The non-capturing loop -- the fix was resolution, not eviction

Added 2026-07-30, resolving the defect section 29 split out of the named-let
report (docs/archive/history/cps-colored-noncapture-named-let-recurses-through-entry.md).

### 31.1 The report guessed the wrong fix, and said so with a precedent

Section 29.3 filed this as "extend the CPS eviction gate", citing the
recursive-await eviction as precedent: a self-recursive shape the direct
emitter's TCO handles in O(1) stack should evict from the CPS path rather than
lower through DK. That reasoning is sound and the precedent is real -- but it
treats the symptom. The question it skips is *why the loop was colored at all*,
given it contains no control operator of any kind.

The answer is the SAME identity skew section 29.2 fixed one layer up. A
captureless letrec lambda binds `go` to a different Binding object than the
lifted `__fn_N`'s own FnDef binding, and records that function's C symbol in
`c_export_name` -- which is precisely how the emitter resolves the call
(`raw_name_for_binding` consults `c_export_name` first; emit_fns.c's self-TCO
check compares the same mangled names). `cps_find_node` compared Binding
POINTERS only. So the self-call resolved to no node, fell into the
"unresolved -> conservatively colored" arm, set `has_indirect`, and colored the
loop.

Everything downstream followed from that one mismatch: colored meant the direct
emitter never saw the function, so CF1 self-TCO never ran; and the CPS
translation, unable to make a `cps->cps` tail call out of a call it could not
resolve either, delegated the body to the direct emitter, whose self-call
targeted the function's own DK ENTRY WRAPPER -- `__dk_entry_depth++`, a
`dk_prompt` malloc, a `setjmp` -- once per iteration.

`cps_find_node` now falls back to C-symbol identity. Two bindings carrying the
same C symbol ARE the same C function (the emitter emits one definition), so
this resolves a real edge rather than widening anything.

### 31.2 Why more precise coloring is not less safe

Un-coloring is the dangerous direction -- an effect that escapes its handler is
a silent miscompile, far worse than an unnecessary DK frame. Three properties
make this change sound, and they are worth stating because they are what makes
the difference between "more precise" and "less conservative":

1. A function that USES a control operator is seeded colored directly
   (`cps_directly_uses_control`). No resolution change can un-color it.
2. Coloring propagates BACKWARD along resolved edges: a conduit that calls a
   colored callee is still colored. Resolving a call ADDS such an edge -- the
   conservative `has_indirect` blanket is only what happens when there is no
   edge to add.
3. The blanket is still there for genuinely unresolvable calls (a call through
   a runtime fn value, an extern not in the node set).

So the functions that stop being colored are exactly those whose callees are
all genuinely uncolored -- the analysis working as designed, on better inputs.

### 31.3 Blast radius: wider than the bug, in the same direction

14 fixtures' `expected.c` moved (regenerated in this change): the four
`letrec-*` / `named-let-*` fixtures whose lowering this is about, and ten
`stackless-catch-unwind-*` whose diffs are stdlib functions no longer being
CPS-emitted at all -- entire `__cps` families (hamt, map, iterator) that were
colored only because a call to an extern-c or non-node callee tripped
`has_indirect`. `letrec-mutual`'s old snapshot carried the exact pathological
shape from 29.3, so mutual letrec was hitting this too.

Every runtime assertion passed before the snapshots were regenerated -- all 14
failures were codegen mismatches, no behavior changed. The effect fixtures keep
their DK lowering (`effect-abort` 143 `__cps` clones, `effects-async` 141),
which property 1 above guarantees.

All three harnesses: `tests/run.sh` **2478 passed / 0 failed**,
`tests/run-jit.sh` (Debug JIT) **2393 / 0 / 47**, `tests/run-turi.sh` **1680
passed / 1 failed** -- the one failure being the pre-existing, separately filed
`hkt-constrained-byvalue-bind-pure`. The benchmark triangle reports no
divergence across interpreter / jit / cc on any program.

### 31.4 The guide's guarantee is now true as written

docs/guides/performance-guide.md listed the named-let idiom under the
self-tail-call guarantee. Section 29.4 had to add a "known gap" bullet for the
non-capturing shape and rewrite the worked example around it. Both shapes are
now optimized, so the bullet is gone and the example needs no caveat; the
boundary list keeps only genuine boundaries. Pinned at 5,000,000 iterations by
`tco-named-let-capture-deep` and `tco-named-let-nocapture-deep`.

Which closes every defect J3 surfaced. The pattern across all four (sections
28-31) is worth keeping: none was caused by the JIT, none was engine-specific
-- gcc and MIR agreed on every one -- and two of the four were fixed by making
an analysis agree with the emitter about IDENTITY, in code where the emitter
already had the answer written down.

## 32. arm64 macOS re-validation of S2/J2/J3 -- one real bug, then parity

Added 2026-07-30 (Apple M2, macOS 27.0). Sections 22-29 had run only on
x86-64 Linux; the last macOS run was `d3eed7d83`. This replays everything
from S2 production forward on Apple Silicon.

Build used: `-DCMAKE_BUILD_TYPE=Debug -DTUR_JIT=ON -DTUR_DEBUG_SANITIZE=OFF`
with Apple clang. See 30.4 -- the toolchain choice is not incidental.

### 32.1 One real bug: c2mir aligns `__uint128_t` to 8

`async-await-channel` and `fiber-scheduler` HUNG under `tur jit` (they pass
under `tur build`). c2mir gives the AArch64 target header's fake
`__uint128_t` (`typedef struct {unsigned long hi, lo;}`) alignment 8 where
AAPCS64 requires 16. That propagates through Apple's NEON-bearing signal
context -- `_STRUCT_ARM_NEON_STATE64` 520/8 vs 528/16, `ucontext_t` 864 vs
880 -- so `FiberBlock` (two embedded `ucontext_t`) comes out 2032 bytes in
the host's view and 2000 in the program's. `tur_fiber_shim` runs
host-resident and writes `f->done` at offset 1776; the JIT-compiled fixture
reads it at 1744, never sees it, and the scheduler re-enqueues forever.

Same shape as 20.3's `TaskGroupBlock`, and exactly the host-runtime/program
seam 20.5 warned diverges. Invisible on x86-64 Linux because nothing in
glibc's `ucontext_t` needs 16-byte alignment.

Report + validated 3-hunk MIR patch:
docs/archive/history/jit-arm64-uint128-align-struct-layout-skew.md (the patch also
fixes two independent c2mir gaps: `_Alignas` is unparseable in
`spec_qual_list`, and ignored for layout when it does parse). LANDED as
fork commit `90633091`, with `TUR_MIR_GIT_TAG` repointed in
`cmake/mir.cmake`. The 30.2 numbers are from a FRESH build directory that
fetches the new pin -- re-verifying in the edited `_deps` tree would have
proved nothing, per the cache trap at `cmake/mir.cmake:33-41`.

### 32.2 Parity after the patch

Measured on this section's base (`5ef07d50a`), i.e. after 30.3 taught
run.sh to descend into group directories AND after 31's CPS coloring
change. An earlier pass of this work recorded 2431 / 0 and 2391 / 1 / 48
against the pre-30.3 harness; those numbers are superseded and are not
comparable to these.

| suite | arm64 macOS | x86-64 Linux |
|---|---|---|
| `tests/run.sh` | **2479 / 0** | 2478 / 0 (section 31.3) |
| `tests/run-jit.sh` | **2393 / 1 / 47**, 60 fallback | 2393 / 0 / 47, 47 fallback |
| `tests/turi/repl-spice-jit.sh` (J2) | **4 / 0** | 4 / 0 |

The run.sh `+1` is not a divergence: 31.3's quoted 2478 predates the
`tco-named-let-nocapture-deep` fixture added in that same commit. On the
base as committed, macOS runs 2479 and the new 5,000,000-iteration TCO
fixture passes on Apple Silicon. (This section's own previous pass, on
`6a63c9d24`, measured 2478 -- exactly one less, the fixture.)

run.sh is otherwise at exact parity, and it stays at parity across 30.3's
+52 fixtures -- the group directories that had never been compiled by the
default suite hold nothing that diverges on Apple Silicon. The run-jit.sh
skip counts match and the single failure is accounted for below. The +13
cc fallbacks are 20.2's Apple SDK header residue, the number that still
has not moved.

The single `run-jit.sh` failure, `httpd-new-pool-fail-drops-handler`, is
NOT a JIT defect: standalone it prints `built` under both AOT and JIT on
macOS. The fixture forces a bind conflict to make `httpd-new-pool` fail,
but BSD `SO_REUSEADDR` permits the second bind where Linux refuses. A
macOS socket-semantics gap in the fixture -- and flaky, since it passed the
AOT suite in the same session.

### 32.3 S2 is live on macOS, and the split has no holes

The 25.1 hash guard is silent on mismatch, so "it works" and "it silently
reverted to full-preamble" look identical from the outside. Checked
directly: `tur emit-rt-split --hash` gives `89d1c5bb0818cb97`, matching the
committed `tur_rt_split_hash`. The preamble is platform-independent; the
split really is taken on macOS.

Full-corpus A/B, `TUR_JIT_NO_SPLIT=1` vs default: **identical** --
2393/1/47 with 60 fallbacks both ways, and byte-identical stdout on a
5-fixture spot check (arith, hamt-basic, stm-stress, dynvar-nested,
self-recursive-carrier-struct-return). No W0071 retries observed. The
resolver-priority hazard 20.5 flagged does not bite the production
arrangement, where the runtime is host-resident and resolved by address
rather than through a dylib's symbol table.

Latency, `arith`, median of 7: **216 ms split vs 246 ms full = 12%**
end-to-end, against Linux's ~28% (25.2). That gap is itself a data point
for the open "size c2mir on macOS" item (plan J2, section 20.4): the split
removes preamble re-parsing, but the ~36 ms of Apple SDK headers (8.3)
sits in the PROGRAM half, which the split cannot touch. Whatever is left
to win on macOS is still in front of the program half, not the preamble.

### 32.4 Toolchain traps that cost two suite runs here

Both produce failures that read exactly like product regressions:

- **Homebrew-LLVM `tur` + Apple-clang fixtures.** CLAUDE.md recommends
  Homebrew LLVM to dodge the ASan startup deadlock, but then `libturi.a`
  carries ASan v8 symbols the system linker cannot resolve:
  `Undefined symbols: ___asan_version_mismatch_check_v8`, reported as
  `build failed` for 46 fixtures. Either pin `CC` to the same clang or
  build `-DTUR_DEBUG_SANITIZE=OFF`. The unsanitized build is cleaner: the
  pinned-`CC` route still shows two spurious httpd failures
  (`httpd-h4-keepalive`, `httpd-h6-routing`) that are ASan artifacts.
- **`cmake --build <dir> --target tur` does not build `libturi.a`.** Every
  fixture then fails to link with `ld: library 'turi' not found`, again
  reported as `build failed`. Build all targets.

## 33. The new MIR pin cross-checked on x86-64, and two harness portability traps

Added 2026-07-30, on top of section 32's arm64 work.

### 33.1 The uint128-alignment pin does not regress x86-64 Linux

Section 32.1's MIR patch was validated on Apple Silicon, where the bug lives.
It also changes struct layout rules for every target, so it needs a check on
the platform it was NOT written for. Fresh build directory (the 32.1 cache trap
is real -- both pre-existing local JIT build dirs still carried
`TUR_MIR_GIT_TAG=41ff4d94` in their CMakeCache and a plain rebuild silently
kept using it; the fresh dir fetches `90633091`):

`tests/run-jit.sh` on x86-64 Linux, new pin: **2394 passed / 0 failed / 47
skipped, 47 cc fallbacks** -- identical to the old pin. No regression.

### 33.2 Two ways the harnesses would have wasted a macOS run

Both are in code added by J3 (section 27), both would have surfaced as broad,
misleading failures rather than clear ones:

- **`tests/run-jit.sh` called bare `timeout(1)`.** Stock macOS ships none --
  Homebrew coreutils installs it as `gtimeout` unless the gnubin path is on
  PATH. `tests/run.sh` has detected this since T19; the JIT harness did not, so
  on a Mac without GNU coreutils every fixture would have died in the RUNNER,
  reading as a total engine failure. Now mirrors run.sh (timeout -> gtimeout ->
  untimed), with the helper exported to the xargs workers.
- **`benchmarks/run-triangle.sh` used `date +%s%N`.** BSD date has no `%N`; it
  emits a literal `N`, which would have poisoned every arithmetic in the script
  silently rather than failing. Now detects that and uses python3 for the
  millisecond clock.

Section 30.3's group-directory scan also used `find -maxdepth 1 -print -quit`,
which is not portable to BSD find; it is a plain glob now. run.sh is otherwise
careful here (`stat -f` before `stat -c`, `sysctl hw.logicalcpu`, the gtimeout
fallback), so the addition was the outlier, not the rule.

None of this was hit by section 32 -- that machine has GNU coreutils on PATH
and the triangle was not part of the re-validation set. They were latent, and
the next macOS run would have paid for them.

## 34. Lazy generation, serialized -- J1's last owed item

Added 2026-07-30. Closes "concurrent-safe or serialized lazy generation"
(8.1), the one thing the J1 phase still owed, and restores section 6's
withdrawn recommendation 5.

### 34.1 Both blockers were measured at a pin three fixes ago

Recommendation 5 ("default to lazy generation") was withdrawn on two defects,
both measured at `a8ab7c31`. The fork is now at `90633091`. Re-measured rather
than inherited:

- **8.4.1 (single-threaded pthread-entry miscompile) does not reproduce.**
  `session-project-basic` and `defstruct-field-session-project` -- the two
  fixtures the finding names -- both print 42 under lazy at the current pin.
- **8.1 (re-entrancy) does.** Full corpus under lazy: 2390 / 4, and all four
  failures are concurrency fixtures (`schan-worker-pool`, `session-effects`,
  `session-mp-effects`, `session-mp-three-role`). Five runs of one of them
  gave three DIFFERENT assertions -- `destroy_func_cfg`, `mark_unreachable_bbs`,
  and `undeclared reg N of func` -- which is a data race with no other
  reading.

That third symptom is 8.4.1's. The likeliest history is that 8.4.1 was always
8.1 seen without the concurrency in view; whatever else moved at the newer pin
just stopped it presenting single-threaded. Worth noting because it means the
corpus regression count (8.4.4's 17 under lazy) was never 17 independent bugs.

### 34.2 Serializing needs no fork patch

MIR-gen runs on one shared `gen_ctx` per context and is not thread-safe.
MIR's `MIR_set_lazy_gen_interface` installs a first-call wrapper that calls
straight into it, so two threads entering two ungenerated functions corrupt it.

Every piece that path uses is public -- `_MIR_get_wrapper`,
`_MIR_redirect_thunk`, `MIR_gen`, and `machine_code` on the func -- so the
interface is reimplemented in `src/jit_engine.c` with a mutex around it.
`MIR_gen` is literally `generate_func_code (ctx, item, TRUE)`, the same call
MIR's own hook makes, so this is MIR's lazy semantics plus mutual exclusion
rather than a second code path. The fork stays clean.

The **double-check is the load-bearing half**. A bare lock still lets two
threads that both cleared the stub generate the same function twice, which is
what trips `_MIR_duplicate_func_insns`; re-reading `machine_code` under the
lock turns the second arrival into a lookup. Contention is self-extinguishing:
a function is generated once, after which its thunk goes straight to the code
and never reaches the hook again.

Result: the fixture that failed 5/5 passes 6/6, and the corpus is **2394 / 0 /
47 -- identical to eager**.

### 34.3 What it buys, and the tradeoff kept in view

Release build, best of 5-7, end-to-end wall:

| program | eager | lazy | saved |
|---|---|---|---|
| fib | 188ms | 144ms | 23% |
| mandel | 190ms | 145ms | 23% |
| loop-sum | 231ms | 147ms | 36% |

Enough to change the headline: with eager, `tur jit` MATCHED the cc round trip
(the reading published in the performance guide at J3); with lazy it beats it
by ~25-30% on all three triangle programs. The guide's table and reading are
updated, with a note that all three legs are re-measured together and are not
comparable to the older snapshot taken on a slower machine.

Lazy is now the default. `TUR_JIT_GEN=eager` is kept, and the reason is worth
recording: **eager doubles as a verification pass.** It generates every
function before any of the program runs, so a generation failure surfaces at
compile time, where cmd_jit's step-6 cc fallback can still catch it. Under lazy
the same failure surfaces at first call -- after output may already have been
written, and past the point where `g_jit_err_active` can unwind to the
fallback. The set of functions that fail is identical in both modes (same
generator, same input; lazy merely skips ones never called), so this is a
question of WHEN, not WHETHER, and no fixture reaches it. If one ever does,
the knob is the diagnostic.

J2's suite (`tests/turi/repl-spice-jit.sh`) is 4/0 under the new default --
the reload loop is where lazy should help most, since a reload regenerates
only what the next call touches.

## 35. The S1b dynamic-variable early-exit edge -- a SEGV, not a leak

Added 2026-07-30. Closes the one edge S1b left open: "an early `return`/`goto`
out of a dynamic binding still pops only on the `cc` path."

### 35.1 Worse than the note suggested

`binding` emits the frame push, then a guard carrying
`__attribute__((cleanup(_dynvar_pop_<name>)))`, then an explicit pop after the
body (findings 12.5 -- the explicit pop exists precisely because c2mir drops
the attribute). An early `return` inside the body jumps past the explicit pop:

```c
TurDynFrame __t161 = { pthread_getspecific (_dynvar_key_lvl), &__t160 };
pthread_setspecific (_dynvar_key_lvl, &__t161);
TurDynFrame *__t162 __attribute__((cleanup(_dynvar_pop_lvl))) = &__t161;
if ((n) > 0) {
    return INT64_C(100);          /* <-- past the pop below */
}
...
_dynvar_pop_lvl (&__t162);
```

On the cc path the attribute still fires, so this is invisible. Under `tur
jit` nothing pops, and the key is left pointing at `__t161` -- a local of a
function that has returned. The next read of the dynamic variable dereferences
a dead stack frame. Measured: cc prints `100 0`; the JIT takes
`AddressSanitizer: SEGV on unknown address 0x0`.

So the recorded severity was too low. "Pops only on the cc path" reads like a
leak; it is a dangling pointer into a reused frame, and the failure is a crash
or a silent wrong read depending on what overwrote the stack.

### 35.2 The fix is the one defers already use

`EX_RETURN` already fires pending defers before returning
(`tur_frame_fire_chain` on `ctx->frame_var`). Dynamic bindings now use the
same shape: `EmitCtx` carries a stack of in-scope dynvar guards, pushed as
each binding opens and popped as it closes, and `EX_RETURN` emits
`_dynvar_pop_<name>(&<guard>)` for every guard still on it, innermost first --
the order the cleanup attribute would have used. The pop is idempotent
(12.5's design), so firing here AND via the attribute on the cc path is safe,
which is what lets one emission serve both engines.

The value-position `EX_RETURN` in emit_expr.c needs no change: it only
materializes the value, and the actual `return` comes from emit_stmt.

One thing the fix had to get right that the leak-checked build caught
immediately: the guard stack's backing arrays outlive emission and have to be
freed at EmitCtx teardown, or `tur build` itself leaks -- the compiler path
runs with LeakSanitizer on.

### 35.3 Coverage

New fixture `dynvar-early-return` pins three shapes: an early return out of
one binding, out of two NESTED bindings (both must pop, innermost first), and
the fall-through path still working. `tests/run.sh` **2480 / 0**;
`tests/run-jit.sh` **2395 / 0 / 47**.

Worth noting what found this: nothing did, for as long as it existed. It was
recorded as a known edge at S1b and sat there because no fixture combined
`binding` with an early `return` -- the same shape of gap as section 30.3's
never-compiled group directories. The engines disagreed the whole time.
