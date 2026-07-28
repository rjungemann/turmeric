# JIT Engine -- Phase J0 spike results

Status: J0 COMPLETE on x86-64 Linux, 2026-07-28.
Plan: [docs/upcoming/jit-engine-plan.md](jit-engine-plan.md)

## 0. Verdict

**MIR works. Proceed to J1.** The `reader -> passes -> emit C -> c2mir ->
MIR-gen -> call` pipeline runs real Turmeric programs in process with no `cc`
subprocess and no disk artifacts, and it does so on 89% of a fixture-corpus
sample without any change to the compiler.

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
is still open. Nothing else in J0 depends on it.

## 1. What was built

| Path | What it is |
|---|---|
| `cmake/mir.cmake` | MIR vendored via `FetchContent`, pinned to `a8ab7c31cd5f9b23b77d84c60b3d83e62d9d304c` (post-v1.0.0). Inert unless `-DTUR_JIT_SPIKE=ON`. |
| `tools/jit-spike/tur-jit-spike.c` | The harness: C text in, `c2mir_compile` -> `MIR_gen` -> call `main`, with per-phase timing. |
| `tools/jit-spike/normalize-c11-subset.py` | Scaffolding that rewrites emitted C into c2mir's subset. Every rule in it is an S1 item; it is deleted when S1 lands. |
| `tools/jit-spike/run-spike.sh` | The J0 exit-criteria set. |
| `tools/jit-spike/sweep-fixtures.sh` | Indicative corpus sample (not J3). |

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

## 6. Recommendations for J1

1. **S1 first, and scope it to three things**: stop emitting `__auto_type`,
   emit `((T)0)` instead of `(T){0}`, emit `_Thread_local` instead of
   `__thread`. That deletes `normalize-c11-subset.py` and takes corpus coverage
   from 89% to ~97% on its own. Expect a full fixture-snapshot regen in the
   same PR.
2. **Emit an explicit `__tur_static_init()`** called from `main` rather than
   relying on `__attribute__((constructor))`. This is a correctness fix for the
   JIT and a legibility win for the `cc` path.
3. **Decide `__attribute__((cleanup))`.** Either lower it explicitly at exit
   edges, or make dynamic variables a documented `cc`-only feature under
   `tur jit` (step-6 fallback with a TUR-W).
4. **Promote S2 ahead of J2**, per section 4.3.
5. **Default to lazy generation** (`MIR_set_lazy_gen_interface`).
6. **Verify arm64 macOS MAP_JIT** before the `EXPERIMENTS[]` row lands. It is
   the one plan gate J0 could not close.
7. The plan's step-6 fallback-to-`cc` is confirmed necessary and sufficient for
   user inline-C. Do not add the `:jit` reader-conditional key from 1.4 -- the
   3 failures in the sample are one stdlib construct, not a pattern.

## 7. Risks, revisited

The plan flagged three MIR risks up front. After J0:

- **Single maintainer / slow cadence** -- unchanged, and unmitigated by
  anything J0 did. The pin is a commit, so we are never surprised.
- **C11 minus atomics/VLAs/complex** -- accurate but incomplete. The costly
  gaps were `__auto_type`, scalar compound literals, and silently discarded
  attributes; no VLA or `_Complex` use was found in generated output at all.
- **Apple Silicon MAP_JIT** -- still unverified. Open gate.

One risk the plan did not list, now the top one: **c2mir fails silently on
constructs it merely ignores.** A parse error is cheap -- it names a line. A
dropped `constructor` attribute cost a SIGSEGV with no diagnostic, and a
dropped `cleanup` produces a plausible wrong answer. J3's parity sweep is
therefore not optional polish; it is the only mechanism that would catch the
next attribute we start emitting.
