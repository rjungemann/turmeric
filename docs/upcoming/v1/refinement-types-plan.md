# Refinement Types -- Prototype Plan (RT0--RT6)

> **Status:** Not started. RT0 syntax/storage is largely covered by the
> existing Contract Types (CT0--CT4) infrastructure; the remaining work is
> the SMT constraint-generation and discharge pipeline (RT1--RT4) plus a
> stdlib layer of predicate-annotated types (RT5--RT6).
>
> **Prerequisites:** Contract Types (CT0--CT4, `-Xcontracts`). The `TY_CONTRACT`
> node, `F_CONTRACT_TYPE` reader tag, and predicate-as-`Form*` storage are
> already in place and reused directly.
>
> **Flag:** `-Xrefinements` (implies `-Xcontracts`). All phases are gated
> behind this flag so the feature can be merged incrementally without breaking
> existing builds.
>
> **Last updated:** 2026-05-21

---

## Motivation

Contract types (`{ x : T | p }`, CT0--CT4) verify predicates at **runtime**.
Refinement types go further: the compiler attempts to **prove** predicates
statically, emitting a runtime check only when static proof fails. This
eliminates whole classes of defensive guards that programmers currently scatter
through the codebase -- e.g. `require! (!= divisor 0)` becomes a type-level
obligation the compiler resolves at every call site.

Goals:

- Catch division-by-zero, out-of-bounds access, and overflow at compile time
  rather than at runtime.
- Allow library authors to publish APIs with machine-checked pre/post conditions
  (e.g. `Vec` indexing, `sqrt` on non-negative floats).
- Compose with the existing substructural and session type systems without
  introducing mutual dependencies.

Non-goals for this prototype:

- Full dependent types (Pi types, proof terms, eliminators).
- Universal or existential quantification inside predicates.
- Termination checking or total-correctness verification.
- Refinements that depend on algebraic effects or mutable state.

---

## Background: What Is Already Shipped

| Component | Location | Notes |
|---|---|---|
| `TY_CONTRACT` type node | `src/compiler/types.h:121` | Stores `{var : base | pred}` |
| `F_CONTRACT_TYPE` reader tag | `src/compiler/reader.c` | Parses `{ x : T \| p }` |
| `type_contract()` constructor | `src/compiler/types.h:936` | Builds a `TY_CONTRACT` from a `Form*` predicate |
| Contract elaboration | `src/compiler/elab_types.c:1072` | Checks predicate is well-typed + pure |
| Runtime check insertion | `src/compiler/elab_core.c` | Emits C `assert` / panic for CT3 |
| `:pre` / `:post` in `defn` | `src/compiler/elab_fns.c` | Parsed and elaborated; currently runtime only |

Refinement types reuse all of the above. The delta is:

1. A **constraint collector** that gathers `TY_CONTRACT` predicates as proof
   obligations during elaboration.
2. An **SMTLIB2 encoder** that translates those obligations to SMT-LIB text.
3. A **libz3 C API driver** (pulled automatically via CPM at build time) that
   submits queries to Z3 in-process and returns `sat`/`unsat` with a model.
4. A **discharge pass** that marks obligations as proven (elide runtime check)
   or unprovable (keep runtime check, optionally warn).
5. A **predicate propagation** layer that infers result refinements from
   argument refinements for simple arithmetic expressions.
6. A **WASM integration layer** that routes queries to the `z3-solver` npm
   package via a JS bridge when Turmeric is running as an Emscripten WASM
   module.

---

## Design Decisions

### Predicate Language Scope

The prototype targets **quantifier-free linear arithmetic over integers and
reals** (QFLIA / QFLRA in SMT-LIB terminology). This covers the vast majority
of useful refinements:

```
pred ::= (= e e) | (!= e e) | (< e e) | (<= e e) | (> e e) | (>= e e)
       | (and pred pred ...) | (or pred pred ...) | (not pred)
       | (=> pred pred)
       | true | false

expr ::= integer-literal | float-literal
       | bound-var                   ;; the variable from { x : T | ... }
       | fn-param                    ;; a visible parameter in scope
       | (+ e e) | (- e e) | (* e const) | (/ e const)  ;; linear only
       | (mod e const)               ;; modular arithmetic
```

Non-linear terms (e.g. `(* x x)`) are **opaque** to the solver -- they are
encoded as uninterpreted constants and can appear in equalities but not in
arithmetic reasoning. The encoder warns when a predicate contains non-linear
subterms.

Higher-order predicates, recursion inside predicates, and effect-dependent
predicates are rejected at elaboration time with a dedicated diagnostic.

### SMT Integration Strategy

#### User Dependency Considerations

A key design constraint is that **Turmeric users should not be required to
manually install Z3**. The options considered differ significantly in their
user-facing setup cost and coverage:

| Option | User setup cost | Binary size delta | Coverage |
|---|---|---|---|
| Z3 subprocess (optional) | Manual `brew install z3` / `apt install z3` | None | Full QFLIA/QFLRA |
| libz3 via CPM (auto-fetched at build time) | None -- CPM downloads and builds it | +15--20 MB (static) | Full QFLIA/QFLRA |
| z3-solver npm (WASM target only) | None -- bundled with web REPL assets | ~5 MB added to WASM bundle | Full QFLIA/QFLRA |
| Inline QFLIA solver (C, no deps) | None | Negligible | Linear integer arithmetic only |

The subprocess-only approach has a compounding problem: if Z3 is absent, the
compiler falls back to runtime checks silently, and users lose static guarantees
without any indication this is happening. That friction is at odds with
Turmeric's zero-setup feel.

#### Decision: libz3 via CPM (Primary Native) + z3-solver npm (WASM) + Inline Solver (Fast Path)

**Native builds:** Z3 officially supports CMake `FetchContent`, exposing a
`z3::libz3` target via a recommended `find_package` → `FetchContent` fallback
pattern. Since Turmeric already uses CPM (which is built on `FetchContent`),
Z3 can be added as a single CPM dependency and downloaded automatically at
build time -- zero user setup. The SMTLIB2 encoder's text output is submitted
to libz3 via `Z3_eval_smtlib2_string`, reusing the encoder without change.
Incremental queries use `Z3_mk_context` / `Z3_push` / `Z3_pop` to avoid
re-parsing the environment on every obligation.

**WASM builds:** The `z3-solver` npm package is Z3 compiled to WebAssembly,
designed to run in-process in a browser or Node.js context -- no subprocess.
Since Turmeric's web REPL is itself an Emscripten module, a thin JS bridge
forwards SMTLIB2 strings from the Turmeric WASM module to `z3-solver` and
returns the result string. This eliminates the `fork`/`execvp` incompatibility
entirely and keeps the same encoder path.

**Inline solver (fast path, optional):** A small Fourier-Motzkin implementation
in C covers trivial linear arithmetic obligations (constant-foldable, single
variable) with near-zero latency -- without any C API call or string serialization.
This is an optimization for the common case, not a dependency-avoidance
strategy. Predicates beyond its scope fall through to libz3 or the JS bridge.
Implemented in RT5c; the system is fully functional without it.

**Subprocess:** used only as a correctness oracle during development (RT0--RT4)
and not shipped as a user-facing path. It is never the primary solver in any
release build.

#### Binary Size

libz3 statically linked adds roughly 15--20 MB. This is non-trivial but
acceptable for a compiler binary; users already accept similar sizes from LLVM-
based tools. The increase can be reduced by building Z3 with unused theories
disabled (`Z3_BUILD_LIBZ3_SHARED=OFF`, `Z3_ENABLE_TRACING=OFF`, etc.). A
shared libz3 is an option for distributions that prefer it.

The WASM bundle cost (~5 MB for z3-solver) is acceptable for the web REPL
given that it is loaded once and cached by the browser.

### Relationship to Contract Types

| | Contract Types (CT) | Refinement Types (RT) |
|---|---|---|
| Predicate syntax | `{ x : T \| p }` | same |
| Verification time | Runtime always | Compile time when possible; runtime fallback |
| SMT involvement | None | Yes, for static discharge |
| Flag | `-Xcontracts` | `-Xrefinements` (implies `-Xcontracts`) |
| `TY_CONTRACT` node | Yes | Same node; adds `rt_discharged` bit |

Refinement elaboration is **additive**: if `-Xrefinements` is off, the
elaborator behaves exactly as with `-Xcontracts`. The discharge pass simply
does not run.

### Interaction with Existing Type System Features

| Feature | Interaction |
|---|---|
| Borrow checker | Refinements on borrowed values are checked at borrow site; no interaction otherwise |
| Linear / affine types | A linear value with a refinement still tracks linearity; refinement discharge is independent |
| Session types | No interaction in prototype |
| HKT / GADTs | Refinements on type parameters are unsupported in prototype; raise a diagnostic |
| Sized types | Sized bounds (SZ0--SZ3) are encoding targets -- `(< i n)` for a `SizedVec n a` index naturally maps to QFLIA |

---

## Architecture Overview

```
src/compiler/
  elab_types.c         -- RT0: attach rt_discharged flag; call constraint collector
  elab_fns.c           -- RT1: collect :pre/:post as obligations; propagation stubs
  refine_collect.c     -- RT1 (new): constraint collector -- walks elaborated forms
  refine_collect.h     -- RT1 (new)
  refine_smtlib.c      -- RT2 (new): SMTLIB2 encoder (shared by all solver backends)
  refine_smtlib.h      -- RT2 (new)
  refine_libz3.c       -- RT3 (new): libz3 C API driver (primary native backend)
  refine_libz3.h       -- RT3 (new)
  refine_discharge.c   -- RT3 (new): discharge pass -- iterates obligations, calls backend
  refine_discharge.h   -- RT3 (new)
  refine_propagate.c   -- RT4 (new): bidirectional predicate propagation
  refine_propagate.h   -- RT4 (new)
  refine_qflia.c       -- RT5c (new): inline Fourier-Motzkin fast path (optional)
  refine_qflia.h       -- RT5c (new)
  diag.h               -- RT3: add TUR-E0370..TUR-E0379 refinement diagnostics
  types.h              -- RT0: add rt_discharged bit to TY_CONTRACT union arm

src/wasm_glue.c        -- RT5a (modified): add JS bridge for z3-solver npm package

CMakeLists.txt         -- RT3 (modified): CPMAddPackage for Z3; z3::libz3 link target

stdlib/
  refine.tur           -- RT5b (new): Nat, Pos, NonZero, Bounded, NonEmpty, Unit
  refine-vec.tur       -- RT5b (new): SizedVec + refinement on index bounds

tests/fixtures/refine/ -- RT5b: end-to-end fixture tests
```

---

## Phase RT0: Infrastructure Hooks

**Goal:** Add the flag, the discharge bit, and wire the discharge pass into the
compilation pipeline. No SMT yet -- all predicates still fall through to
runtime checks.

### Changes

**`types.h`** -- add `rt_discharged` to the `TY_CONTRACT` arm:

```c
struct {
    struct Type        *base_type;
    const char         *var_name;
    const struct Form  *predicate;
    bool                rt_discharged; /* RT0: true = SMT proved; elide runtime check */
} contract_;
```

**`CMakeLists.txt`** -- add `-Xrefinements` to the recognised `-X` flags list,
set `g_opt_refinements` (mirrors the existing pattern for `-Xcontracts`,
`-Xsessions`, etc.).

**`elab_types.c`** -- after the existing contract elaboration block, add:

```c
if (g_opt_refinements)
    refine_collect_obligation(e, ct, pred_form, source_loc);
```

**Compilation pipeline** (`main.c` or wherever passes are sequenced) -- after
all elaboration is complete, run:

```c
if (g_opt_refinements)
    refine_discharge_all(compilation_unit);
```

### Acceptance Criteria

- `just build` succeeds with no new warnings (CPM fetches Z3 automatically).
- `-Xrefinements` is accepted and behaves identically to `-Xcontracts` until
  RT3 wires in the discharge pass.
- Existing contract tests continue to pass under both flags.

---

## Phase RT1: Constraint Collector

**Goal:** Implement `refine_collect.c`. This pass walks the post-elaboration
form tree and records each `TY_CONTRACT` crossing point as a **proof
obligation**.

### Obligation Record

```c
typedef struct RefineObligation {
    const Form    *predicate;   /* the p in { x : T | p } */
    const char    *var_name;    /* the x */
    Type          *base_type;   /* the T */
    SourceLoc      loc;         /* where the crossing occurs */
    RefineEnv     *env;         /* in-scope refinements at this point */
    bool           discharged;  /* set by RT3 discharge pass */
    bool           proven;      /* set by RT3: true = SMT proved unsat of negation */
    const char    *counterex;   /* RT3: Z3 counterexample string if not proven */
} RefineObligation;

typedef struct RefineObligation RefineObligationVec[];  /* arena-backed array */
```

### Crossing Points to Collect

| Situation | Obligation |
|---|---|
| Passing an `expr : T` where `T` is a `TY_CONTRACT { x : U \| p }` | Prove `p[expr/x]` holds given current env |
| Returning from a `defn` with `:post r` | Prove `:post` holds for the return value |
| Struct field write where field type is `TY_CONTRACT` | Prove field predicate holds |
| Pattern-match arm narrowing a refined type | Add the predicate to the env for that arm |

### Environment

The `RefineEnv` is a lightweight chain of `(name, predicate)` pairs pushed at
each lexical boundary:

```c
typedef struct RefineEnvEntry {
    const char           *name;      /* variable name */
    const Form           *predicate; /* known-true predicate mentioning name */
    struct RefineEnvEntry *next;
} RefineEnvEntry;

typedef struct RefineEnv {
    RefineEnvEntry *head;
    Arena          *arena;
} RefineEnv;
```

Inside a conditional `(if (> x 0) <then> <else>)`, the then-branch environment
gets `(x > 0)` added; the else-branch gets `(not (> x 0))`.

### Acceptance Criteria

- A new test `tests/unit/refine_collect_test.c` drives the collector on a
  handful of hand-constructed forms and asserts the correct set of obligations
  is generated.
- No obligations are collected when `-Xrefinements` is off.

---

## Phase RT2: SMTLIB2 Encoder

**Goal:** Implement `refine_smtlib.c`. Given a `RefineObligation` and its
`RefineEnv`, emit a self-contained SMTLIB2 string that Z3 can consume.

### Encoding Strategy

Each obligation is encoded as:

```
(set-logic QF_LIA)           ; or QF_LRA for float predicates
(declare-const x Int)        ; the bound variable
(declare-const a Int)        ; each in-scope parameter that appears in predicates
...
(assert <env-predicate-1>)   ; known facts from RefineEnv
(assert <env-predicate-2>)
...
(assert (not <goal>))        ; negate the goal -- unsat means goal is proved
(check-sat)
(get-model)                  ; only reached if sat (counterexample)
```

### Encoding Rules

| Turmeric Form | SMTLIB2 Output |
|---|---|
| `(= e1 e2)` | `(= <e1> <e2>)` |
| `(!= e1 e2)` | `(not (= <e1> <e2>))` |
| `(< e1 e2)` | `(< <e1> <e2>)` |
| `(<= e1 e2)` | `(<= <e1> <e2>)` |
| `(> e1 e2)` | `(> <e1> <e2>)` |
| `(>= e1 e2)` | `(>= <e1> <e2>)` |
| `(and p q)` | `(and <p> <q>)` |
| `(or p q)` | `(or <p> <q>)` |
| `(not p)` | `(not <p>)` |
| `(=> p q)` | `(=> <p> <q>)` |
| `(+ e1 e2)` | `(+ <e1> <e2>)` |
| `(- e1 e2)` | `(- <e1> <e2>)` |
| `(* e const)` | `(* <e> <const>)` -- linear multiplication only |
| `(mod e const)` | `(mod <e> <const>)` |
| integer literal | decimal string |
| float literal | decimal string; switches logic to QF_LRA |
| name (in scope) | declared constant |
| non-linear `(* x x)` | fresh uninterpreted constant + warning |

The encoder is a recursive `Form*` walker. It writes into an arena-backed
`StringBuilder` to avoid heap fragmentation.

### Acceptance Criteria

- A unit test encodes a handful of obligations and compares the SMTLIB2 output
  string against expected fixtures.
- Non-linear subterms produce `TUR-W0370: non-linear predicate subterm -- SMT
  reasoning may be incomplete` and fall back to an uninterpreted constant.

---

## Phase RT3: libz3 C API Driver and Discharge Pass

**Goal:** Implement `refine_libz3.c` (libz3 C API wrapper) and
`refine_discharge.c` (iterate over all obligations, call the solver, mark
results). Wire Z3 into the build via CPM so users need not install anything.

### CMake / CPM Integration

In `CMakeLists.txt`, add Z3 as a CPM package using the recommended
`find_package` → `FetchContent` fallback pattern:

```cmake
# Try a system-installed Z3 first; fall back to CPM download.
find_package(Z3 4.12 CONFIG QUIET)
if(NOT Z3_FOUND)
  CPMAddPackage(
    NAME z3
    GITHUB_REPOSITORY Z3Prover/z3
    GIT_TAG z3-4.13.0
    OPTIONS
      "Z3_BUILD_LIBZ3_SHARED OFF"
      "Z3_BUILD_PYTHON_BINDINGS OFF"
      "Z3_BUILD_JAVA_BINDINGS OFF"
      "Z3_ENABLE_TRACING OFF"
      "Z3_ENABLE_EXAMPLE_TARGETS OFF"
  )
endif()

target_link_libraries(turi PRIVATE z3::libz3)
```

This is gated on `g_opt_refinements` -- if `-Xrefinements` is never enabled at
build time, Z3 is not fetched or linked.

### libz3 Driver

```c
typedef enum Z3Result { Z3_UNSAT, Z3_SAT, Z3_UNKNOWN, Z3_ERROR } Z3Result;

/* One Z3_context is created per compilation unit and reused across all
 * obligations. Z3_push/Z3_pop isolate each query's scope. */
typedef struct RefineZ3Ctx {
    Z3_context  ctx;
    Z3_solver   solver;
} RefineZ3Ctx;

RefineZ3Ctx  *refine_z3_ctx_new(Arena *a);
void          refine_z3_ctx_free(RefineZ3Ctx *c);

/* Submits smtlib_text via Z3_eval_smtlib2_string; returns result and,
 * if SAT, writes the model text into out_model (arena allocation). */
Z3Result refine_z3_query(RefineZ3Ctx *c, const char *smtlib_text,
                          Arena *a, char **out_model);
```

Using `Z3_eval_smtlib2_string` means the SMTLIB2 encoder (RT2) is reused
without change. Incremental queries push/pop the solver context to avoid
re-asserting environment facts for each obligation.

### Discharge Pass

```c
void refine_discharge_all(RefineObligationVec *obligations, Arena *a, DiagCtx *diag);
```

For each obligation:

1. Call `refine_smtlib_encode()` to produce the SMTLIB2 text.
2. Optionally try the inline fast path (RT5c) for trivial linear obligations.
3. Call `refine_z3_query()` for anything the fast path cannot decide.
4. If `Z3_UNSAT`: set `obligation->proven = true`, `obligation->discharged = true`.
   The `TY_CONTRACT` node's `rt_discharged` bit is set; codegen emits no
   runtime check.
5. If `Z3_SAT`: set `obligation->proven = false`, store the model text in
   `obligation->counterex`. Emit `TUR-E0371`. Runtime check is kept.
6. If `Z3_UNKNOWN` or `Z3_ERROR`: emit `TUR-W0372`. Runtime check is kept.

### New Diagnostics

| Code | Kind | Message Template |
|---|---|---|
| `TUR-E0370` | Error | `refinement predicate is ill-typed: <reason>` |
| `TUR-E0371` | Error | `refinement predicate cannot be proved statically; counterexample: <model>` |
| `TUR-W0372` | Warning | `solver returned unknown for refinement predicate at <loc>; runtime check kept` |
| `TUR-W0373` | Warning | `non-linear predicate subterm '<subterm>'; SMT reasoning may be incomplete` |
| `TUR-E0375` | Error | `refinement predicate mentions effects; pure predicates only` |
| `TUR-E0376` | Error | `refinement on type parameter is not supported in this prototype` |

### Acceptance Criteria

End-to-end tests in `tests/fixtures/refine/`:

```turmeric
;; refine/proved.tur -- should compile cleanly, no runtime check emitted
(defn double-pos [x : { v : int | (> v 0) }] : { r : int | (> r 0) }
  (* x 2))

;; refine/unproved.tur -- should emit TUR-E0371 with counterexample
(defn wrong [x : int] : { r : int | (> r 0) }
  x)

;; refine/float.tur -- float predicate discharged via QF_LRA
(defn sqrt-pos [x : { v : double | (>= v 0.0) }] : double
  (sqrt x))
```

- `just build` fetches Z3 automatically via CPM with no user intervention.
- All three fixtures behave correctly on a fresh machine with no Z3 pre-installed.

---

## Phase RT4: Bidirectional Predicate Propagation

**Goal:** Implement `refine_propagate.c` to infer result refinements from
argument refinements for arithmetic expressions, reducing the burden on the
programmer.

### Motivation

Without propagation, a programmer must manually annotate:

```turmeric
(defn inc-pos [x : { v : int | (> v 0) }] : { r : int | (> r 0) }
  (+ x 1))
```

With propagation, the result type `{ r : int | (> r 0) }` can be inferred
automatically from the argument type `{ v : int | (> v 0) }` and the body
`(+ x 1)`, because the solver can prove `(> x 0) => (> (+ x 1) 0)`.

### Approach: Template-Based Inference (Liquid Types)

Rather than full predicate synthesis, the prototype uses **predicate
templates**: a fixed set of predicate shapes are tried against each return
expression, and the solver checks which ones are implied by the argument
refinements. Templates are drawn from:

1. Predicates appearing on the function's parameters.
2. A small built-in vocabulary: `(> r 0)`, `(>= r 0)`, `(< r 0)`, `(<= r 0)`,
   `(!= r 0)`, `(= r <literal>)`.

The elaborator tries each template in order; the first one that is provable
under the current environment is used as the inferred result refinement. If no
template is provable and the programmer did not write an explicit return
refinement, the return type carries no refinement (same as today).

This is conservative (may miss some provable refinements) but sound (never
infers a false refinement).

### Scope

Propagation applies only to:

- Single-expression function bodies (no branching in the body).
- Arithmetic expressions over the function's direct parameters.
- Functions with at most 4 refined parameters (combinatorial explosion
  otherwise; raise `TUR-W0377: too many refined parameters for propagation`).

Branching bodies require path-sensitive propagation (joining refinements at
merge points) -- deferred to a follow-up phase.

### Acceptance Criteria

```turmeric
;; refine/propagate.tur

;; Inferred: result is (> r 0) -- no annotation needed on return type
(defn inc-pos [x : { v : int | (> v 0) }] : int
  (+ x 1))

;; Inferred: result is (!= r 0) -- propagated from argument
(defn double-nonzero [x : { v : int | (!= v 0) }] : int
  (* x 2))
```

Both should compile without `TUR-E0371` and with the inferred refinements
reflected in the type printed by `turi --print-types`.

---

## Phase RT5a: WASM Integration

**Goal:** Enable static refinement discharge in the Turmeric web REPL, where
the runtime is an Emscripten WASM module and libz3 subprocess/linking is
unavailable. Uses the `z3-solver` npm package (Z3 compiled to WASM, running
in-process in the browser) via a thin JS bridge.

### Architecture

```
Turmeric WASM module (C/Emscripten)
  └─ refine_discharge_all()
       └─ refine_wasm_query(smtlib_text)   [Emscripten JS interop]
            └─ window.__tur_z3_query(smtlib_text)   [JS bridge]
                 └─ z3-solver npm package (z3.wasm, runs in same JS context)
```

The `z3-solver` package exposes a Promise-based API. Since the Turmeric WASM
module calls into JS synchronously, the bridge uses a `SharedArrayBuffer` +
`Atomics.wait` pattern (available in cross-origin isolated contexts) to block
the WASM thread while the JS side resolves the Promise. This is the same
technique used by Emscripten's `ASYNCIFY` for synchronous JS interop.

### Changes

**`src/wasm_glue.c`** -- add:

```c
#ifdef __EMSCRIPTEN__
/* Defined in web/z3-bridge.js; submits smtlib_text to z3-solver and
 * writes the result ("sat\n...", "unsat", or "unknown") into out_buf. */
extern void tur_z3_query_sync(const char *smtlib_text, char *out_buf, int buf_len);

Z3Result refine_wasm_query(const char *smtlib_text, Arena *a, char **out_model) {
    char buf[65536];
    tur_z3_query_sync(smtlib_text, buf, sizeof(buf));
    return parse_z3_result(buf, a, out_model);
}
#endif
```

**`web/z3-bridge.js`** (new) -- loads `z3-solver`, implements
`tur_z3_query_sync` using `Atomics.wait` to make the async API appear
synchronous to the WASM side:

```js
import { init } from 'z3-solver';

const z3 = await init();

// Called from Emscripten via ccall; blocks WASM thread until Z3 responds.
globalThis.__tur_z3_query = function(smtlib) {
  const result = z3.evalSmtlib2String(smtlib);
  return result;
};
```

**`web/package.json`** -- add `"z3-solver": "^4.13.0"` to dependencies.

**`CMakeLists.txt` / `justfile`** -- `just wasm` bundles `web/z3-bridge.js`
alongside `turmeric.js` and configures the Emscripten build with
`-s SHARED_MEMORY=1 -s PTHREAD_POOL_SIZE=1` for `Atomics.wait` support.

### Acceptance Criteria

- The web REPL at `localhost:PORT` (via `just web-dev`) successfully discharges
  the `refine/proved.tur` example statically (no runtime check inserted).
- The `refine/unproved.tur` example surfaces `TUR-E0371` in the REPL output.
- Page load time regression is under 500ms (z3-solver is loaded lazily on
  first refinement query, not at startup).

---

## Phase RT5b: Standard Library Refinement Types

**Goal:** Add `stdlib/refine.tur` with common predicate-annotated type aliases,
usable as drop-in replacements for raw numeric types. Can proceed in parallel
with RT5a once RT3 is complete.

### `stdlib/refine.tur`

```turmeric
;;; Nat -- non-negative integer (>= 0)
(deftype Nat { x : int | (>= x 0) })

;;; Pos -- strictly positive integer (> 0)
(deftype Pos { x : int | (> x 0) })

;;; NonZero -- integer that is not zero
(deftype NonZero { x : int | (!= x 0) })

;;; Bounded -- integer in [lo, hi] (inclusive)
(deftype (Bounded lo hi) { x : int | (and (>= x lo) (<= x hi)) })

;;; PosFloat -- non-negative float
(deftype PosFloat { x : double | (>= x 0.0) })

;;; NonEmpty -- a Vec with at least one element (deferred to RT5b once Vec
;;;             refinement is wired through SizedVec)
```

### `stdlib/refine-vec.tur`

Extends `stdlib/gadt-vec.tur` (SZ0--SZ3) to wire refinement type obligations
through vector indexing:

```turmeric
;;; refine-vec/get -- index a SizedVec with a statically-bounded index
;;;
;;; The predicate (< i n) is a refinement obligation that the SMT layer
;;; will attempt to discharge at compile time.
(defn refine-vec/get [v : (SizedVec n a), i : { j : int | (< j n) }] : a
  (vec-unsafe-get v i))
```

The call to `vec-unsafe-get` is only emitted when the `(< j n)` obligation is
proved statically. Otherwise the elaborator falls back to a bounds-checked
`vec-get` with a runtime panic.

### Acceptance Criteria

```turmeric
(import refine)
(import refine-vec)

(defn safe-div [n : int, d : NonZero] : int
  (/ n d))

(defn main [] :void
  ;; This should compile and run with no runtime check for the divisor
  (println (safe-div 10 2)))
```

No runtime contract check is emitted for `(/ n d)` because `d : NonZero`
provides `(!= d 0)` and the elaborator proves the SMT obligation statically.

---

## Phase RT5c: Inline QFLIA Fast Path (Optional)

**Goal:** Implement `refine_qflia.c` -- a small Fourier-Motzkin solver for
trivial linear arithmetic obligations. This is a **performance optimization**,
not a dependency-avoidance mechanism; libz3 already covers the zero-setup case.

The motivation is latency: for obligations that are obviously true or false
(single variable, constant bounds), calling into libz3 involves C API overhead
and string serialization. The inline fast path decides these in microseconds
with no allocation.

### Scope

The fast path handles:

- Single-variable inequalities: `(> x 0)`, `(<= x 100)`, `(!= x 0)`, etc.
- Conjunctions of single-variable inequalities.
- Obligations whose environment contains only single-variable facts.

Everything else falls through to libz3 unchanged.

### Cross-Validation

In debug builds (`-DCMAKE_BUILD_TYPE=Debug`), every obligation the fast path
decides is also sent to libz3 and results are compared. Disagreements emit
`TUR-I0379: inline fast path disagrees with Z3 -- please file a bug` and are
treated as `Z3_ERROR` (runtime check kept).

### Acceptance Criteria

- The fast path correctly decides all single-variable obligations in the RT5b
  fixtures without calling libz3.
- `TUR_REFINE_STATS=1` env var prints per-compilation counts of fast-path
  hits vs. libz3 calls, enabling empirical tuning.
- RT5c is entirely optional: removing `refine_qflia.c` from the build produces
  identical results with slightly higher compile times.

---

## Phase RT6: Error Message Quality

**Goal:** Make refinement type errors actionable. A raw Z3 counterexample is
opaque; this phase wraps it in source-language terms.

### Counterexample Translation

When Z3 returns `sat` with a model like:

```
(model
  (define-fun x () Int 0)
  (define-fun y () Int -1))
```

The discharge pass translates this back to Turmeric syntax:

```
error[TUR-E0371]: refinement predicate cannot be proved statically
  --> src/myfile.tur:14:3
   |
14 |   (defn inc [x : int] : { r : int | (> r 0) }
   |                          ^^^^^^^^^^^^^^^^^^^^
   |
   = counterexample: x = 0, expected (> (+ x 1) 0) but got (> 1 0) = true
   
   note: the predicate (> r 0) is not always satisfied when x may be 0
   hint: use { x : int | (>= x 0) } or { x : int | (> x 0) } as the
         parameter type to constrain x
```

Key elements:
- Source location of the failing predicate.
- Concrete variable assignments from the counterexample.
- A plain-English restatement of what went wrong.
- A hint suggesting the missing precondition, derived by checking which
  additional predicate on the input would make the goal provable (a second Z3
  query with `(forall (x Int) (=> <candidate> <goal>))`).

### Acceptance Criteria

- `tests/fixtures/refine/error-messages/` contains snapshots of expected
  diagnostic output for 5 representative failing obligations.
- `just test` runs those snapshots through the compiler and diffs against
  expected output (same mechanism used by the session types snapshot tests).

---

## Implementation Order and Dependencies

```
RT0  (infrastructure hooks + CPM/Z3 build wiring)
 |
RT1  (constraint collector)      RT2  (SMTLIB2 encoder)
 |                                |
 +--------------------------------+
 |
RT3  (libz3 C API driver + discharge pass)
 |
RT4  (predicate propagation)     RT5a (WASM / z3-solver JS bridge)
 |                                |
 |                               RT5b (stdlib types; refine.tur / refine-vec.tur)
 |                                |
 +----------------+---------------+
                  |
                 RT5c (inline fast path; optional)
                  |
RT6  (error message quality)
 |
RT7  (incremental discharge caching; follow-up)
```

RT2 can be developed and unit-tested independently of RT1. RT5a and RT5b can
be developed in parallel once RT3 is complete. RT5c is optional and can be
deferred indefinitely without affecting correctness. RT7 is a follow-up phase
that does not block any earlier work.

---

## Effort Estimates

| Phase | Estimated Effort | Notes |
|---|---|---|
| RT0 | 1 day | Flag wiring, bit field, CPM/Z3 build integration |
| RT1 | 2 days | Core collector; env chain; crossing point detection |
| RT2 | 2 days | SMTLIB2 encoder; unit tests against expected strings |
| RT3 | 2 days | libz3 C API driver; discharge pass; diagnostics |
| RT4 | 3 days | Template-based propagation; arithmetic pattern matching |
| RT5a | 2 days | WASM JS bridge; z3-bridge.js; Emscripten build flags |
| RT5b | 1.5 days | `refine.tur`; `refine-vec.tur`; integration tests |
| RT5c | 2 days | Inline fast path; cross-validation harness (optional) |
| RT6 | 2 days | Counterexample translation; hint generation; snapshot tests |
| RT7 | 2 days | Incremental caching; predicate hash keying (follow-up) |
| **Total (without RT5c, RT7)** | **~15.5 days** | Assumes one developer |
| **Total (with RT5c, RT7)** | **~19.5 days** | RT5c and RT7 are optional follow-ups |

---

## Phase RT7: Incremental Discharge Caching (Follow-up)

**Goal:** Avoid re-running refinement queries on files that have not changed
since the last build. Currently the discharge pass runs on every compilation
regardless of whether any source has changed, which is wasteful for large
codebases.

### Design

Each `RefineObligation` is keyed by a hash of:
- The encoded SMTLIB2 text (captures the predicate and environment fully).
- The Turmeric compiler version (invalidates cache across releases).

Results (`proven`, `counterex`) are stored in a per-project cache file
(e.g. `.tur-cache/refine.db`, a flat key-value store using a simple binary
format or SQLite). On subsequent builds, obligations whose hash matches a
cached entry skip the libz3 call entirely.

Cache invalidation is conservative: any change to the source file containing
the obligation evicts all of that file's cached entries. This is overly broad
but safe and simple; finer-grained invalidation can be added later.

### Acceptance Criteria

- A second `just build` with no source changes produces zero libz3 calls
  (verified via `TUR_REFINE_STATS=1`).
- Modifying a source file evicts exactly that file's obligations from the cache.
- The cache file is safe to delete at any time (`just clean` removes it);
  the build falls back to full discharge transparently.

---

## Open Questions

1. **Z3 version pinning.** ✅ *Decided.* Prefer a system-installed Z3 if it
   meets the minimum version (4.12); use CPM as the automatic fallback. If
   `find_package` finds a system Z3 that is older than 4.12, emit a hard error
   (`TUR-E0380: system Z3 version <ver> is below the required minimum 4.12 --
   uninstall it or set Z3_DIR to the CPM-built copy`) rather than silently
   falling back to behaviour that may produce wrong results.

2. **WASM cross-origin isolation.** ✅ *Decided.* Use the `Atomics.wait`
   synchronous bridge (same UX as native -- discharge happens inline). The
   required `Cross-Origin-Opener-Policy: same-origin` and
   `Cross-Origin-Embedder-Policy: require-corp` headers are already needed for
   Emscripten thread support. Verify that `just web-dev` and any production
   hosting set both headers correctly as part of the RT5a acceptance criteria.

3. **CPM build time.** ✅ *Decided.* Use pre-built libz3 binaries from the Z3
   GitHub Releases page for CI. The CPM call in `CMakeLists.txt` gains a
   platform-detection branch:

   ```cmake
   if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
     set(Z3_PREBUILT_URL  "https://github.com/Z3Prover/z3/releases/download/z3-4.13.0/z3-4.13.0-arm64-osx-13.7.zip")
     set(Z3_PREBUILT_SHA256 "<sha256>")
   elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
     set(Z3_PREBUILT_URL  "https://github.com/Z3Prover/z3/releases/download/z3-4.13.0/z3-4.13.0-x64-glibc-2.35.zip")
     set(Z3_PREBUILT_SHA256 "<sha256>")
   endif()
   CPMAddPackage(URL ${Z3_PREBUILT_URL} URL_HASH SHA256=${Z3_PREBUILT_SHA256})
   ```

   Developer machines still build from source via the `GITHUB_REPOSITORY` path
   (once, then cached). CI always hits the pre-built binary path.

4. **Incremental compilation.** *Promoted to RT7.* See phase RT7 below.

5. **`:post` with mutable references.** ✅ *Decided.* Allow `:post` on functions
   that take `&mut T` parameters, but reject any predicate whose free variables
   include an `&mut` parameter name. The elaborator checks the predicate's free
   variables at the point of `:post` elaboration:

   - If the predicate mentions `result` and/or non-`&mut` parameters only:
     allowed. Example: `(>= result 0)` on a function taking `&mut Vec` is fine.
   - If the predicate names any `&mut` parameter: emit
     `TUR-E0378: post-condition predicate references mutable parameter '<name>';
     only the return value ('result') and non-mutable parameters may appear`.

   This is a free-variable check on the predicate `Form*` and requires no
   alias analysis.

6. **Interaction with typeclasses.** ✅ *Decided.* Reject refined types on
   typeclass method signatures in the prototype with `TUR-E0376: refinement on
   typeclass method return type is not supported in this prototype`. Per-instance
   discharge is deferred to a follow-up phase.

---

## References

- **Liquid Haskell** -- Rondon, Kawaguchi, Jhala. *Liquid Types* (PLDI 2008).
  The template-based inference approach in RT4 follows this paper closely.
- **F*: Fully Abstract Compilation** -- Swamy et al. The two-phase
  (static-then-runtime-fallback) strategy is modelled on F*'s `Tot`/`Dv`
  effect distinction.
- **Dafny** -- Leino. *Dafny: An Automatic Program Verifier for Functional
  Correctness* (LPAR 2010). The counterexample hint generation in RT6 mirrors
  Dafny's error reporting.
- **Z3 SMT-LIB2 reference** -- `z3 --help` and the SMT-LIB 2.6 standard
  at smtlib.cs.uiowa.edu.
- **z3-solver npm package** -- Z3 compiled to WebAssembly; used for the WASM
  REPL backend (RT5a). Published by the Z3Prover team at npmjs.com/package/z3-solver.
- **Z3 CMake integration** -- `Z3Prover/z3: README-CMake.md`; documents the
  `find_package` / `FetchContent` hybrid pattern used in RT3.
- **Turmeric Contract Types** -- `docs/guides/contract-types-guide.md`
- **Turmeric Sized Types** -- `docs/guides/sized-types-guide.md` and CT0--CT4
  in `docs/guides/advanced-type-system-rationale.md`
