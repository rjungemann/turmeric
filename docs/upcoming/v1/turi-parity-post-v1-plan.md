# turi ↔ tur Parity (post-v1) Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-06-10
> **Type:** Interpreter / Test Infra / Docs
> **Scope:** post-v1 -- not blocking the v1 release

---

## Overview

The tree-walking interpreter `turi` (`src/turi/eval.c`, `libturi`) backs
`tur run`, `tur repl`, sandbox eval (`stdlib/turi/eval.tur`), and the
"interpreter equivalent" leg of the test harness (`tests/run-turi.sh`).
It is intentionally *not* a separate language -- it walks the same `Expr`
tree that the C codegen lowers. Every expression kind the compiler can
emit, the interpreter should be able to evaluate (modulo a small,
documented escape-hatch list).

Today there is a real, measurable gap:

- **Expression kinds:** 36 of 114 `EX_*` kinds defined in
  `src/compiler/expr.h` have **no case arm** in `src/turi/eval.c`. The
  default arm returns `eval: unhandled expression kind N (not yet
  implemented in interpreter)` (`src/turi/eval.c:3725-3727`).
- **Fixture coverage:** `tests/run-turi.sh` runs a hand-curated allowlist
  of **120 fixtures out of 1,268** in `tests/fixtures/` (~9.5% -- the
  gap has *widened* since the 2026-06-01 draft, as ~250 new fixtures
  have landed without entering the allowlist). Anything not in the
  allowlist emits `SKIP %s (not in turi allowlist)` rather than failing
  -- so allowlist gaps are silent.
- **Documented limitation:** The `eval-api` guide
  (`docs/guides/eval-api.md:77`) only notes that inline-C and a subset of
  async builtins are disabled in sandbox mode. There is no published
  parity matrix for the full interpreter.

A previous pass at this work landed as
`docs/archive/interpreter-features-plan.md` (May 2026) and shipped most
of Phase S4 (pattern matching, instance registration, module loading,
macro expansion in the REPL). This post-v1 plan picks up where that left
off and drives toward a **published parity matrix** plus a CI gate that
prevents the gap from growing.

---

## Goals / Non-Goals

### Goals

- Implement, or explicitly carve out as documented escape hatches, every
  `EX_*` kind the compiler emits.
- Replace the hand-curated `TURI_FIXTURES_DEFAULT` allowlist in
  `tests/run-turi.sh` with a **denylist** keyed off a per-fixture marker
  (`requires.compiled` or a new `requires.tur-only`), so new fixtures
  default to being run under turi.
- Publish a parity matrix at `docs/guides/turi-parity-guide.md` listing
  every language feature, marking it `tur` only, `turi` only, or both,
  with a one-line rationale for each interpreter-only carve-out.
- Add a CI assertion that the count of `EX_*` kinds missing from
  `src/turi/eval.c` does not regress (a small Python script in
  `tools/check_turi_parity.py`).
- Land enough fixture coverage to push the turi harness from ~12% to
  **>=80% of `tests/fixtures/`** -- modulo the documented carve-outs.

### Non-Goals

- A self-hosted interpreter (was Phase S6; explicitly out of scope and
  remains so).
- Bytecode compilation. `turi` stays a tree walker; the JIT-like
  optimisations live in the `tur` C codegen path.
- Performance parity. The interpreter is allowed to be 10-100x slower
  than compiled code. This plan does not target speed; it targets
  *behaviour* parity.
- Inline-C execution. `EX_INLINE_C` stays a documented limitation
  (`src/turi/eval.c:3141-3145`). The existing native-override mechanism
  in `eval.c:2427-2455` (the `try_exec_simple_inline_c` pattern matcher
  for malloc/field-store/field-load shapes) covers the stdlib cases that
  need it; user inline-C requires the compiled path.
- Async parity for WASM. The fiber scheduler is intentionally stubbed in
  `src/turi/fiber.c:172-288` for Emscripten and stays that way.

---

## Current State (from static analysis)

### Expression kinds with no `case` arm in `src/turi/eval.c`

```
EX_ATOMICALLY            EX_OR_ELSE               EX_STM
EX_CALLCC                EX_PANIC_PAYLOAD_DOWNS   EX_SYM_LIT
EX_CATCH_PANIC_OF        EX_PANIC_PAYLOAD_FILE    EX_TVAR_CAS
EX_CHECK                 EX_PANIC_PAYLOAD_LINE    EX_TVAR_MODIFY
EX_CLONEABLE_RESET       EX_PANIC_PAYLOAD_TYPE    EX_TVAR_NEW
EX_CLONEABLE_SHIFT       EX_PANIC_PAYLOAD_VALUE   EX_TVAR_READ
EX_COMPOSE_HANDLERS      EX_RESET                 EX_TVAR_SWAP
EX_CONS_LIST             EX_RETRY                 EX_TVAR_WRITE
EX_CPS_CONT_APP          EX_SELECT                EX_WITH_HANDLER
EX_GEN                   EX_SERIAL_SHIFT          EX_YIELD
EX_GEN_DONE              EX_SET_FIELD
EX_GEN_NEXT              EX_SHIFT
EX_HANDLER_LIT
EX_LETREC
```

`EX_SERIAL_RESET` is handled but only as a "not yet implemented" error
(`src/turi/eval.c:3720-3722`); it is functionally in the same bucket.

`EX_SHIFT0` *was* on the original list and **has since landed** -- one
of two kinds the interpreter picked up since 2026-06-01. The other new
entries `EX_CALLCC` and `EX_CPS_CONT_APP` did not exist when the plan
was drafted; both come from the CPS-transform work and should be tackled
alongside it (see Risks #5 and Sequencing).

### Categorised

1. **Trivial / quick wins (no design work):** `EX_LETREC`, `EX_SYM_LIT`,
   `EX_CONS_LIST`, `EX_SET_FIELD`, `EX_HANDLER_LIT`,
   `EX_COMPOSE_HANDLERS`, `EX_OR_ELSE`, `EX_CHECK`.
2. **Generators:** `EX_GEN`, `EX_GEN_NEXT`, `EX_GEN_DONE`, `EX_YIELD`.
3. **Delimited control:** `EX_SHIFT`, `EX_RESET`,
   `EX_CLONEABLE_SHIFT`, `EX_CLONEABLE_RESET`, `EX_SERIAL_SHIFT`,
   `EX_SERIAL_RESET`. (`EX_SHIFT0` is now implemented.)
4. **STM:** `EX_STM`, `EX_ATOMICALLY`, `EX_RETRY`, `EX_TVAR_NEW`,
   `EX_TVAR_READ`, `EX_TVAR_WRITE`, `EX_TVAR_MODIFY`, `EX_TVAR_SWAP`,
   `EX_TVAR_CAS`.
5. **Panic payloads (depends on `catch-unwind` landing in tur):**
   `EX_CATCH_PANIC_OF`, `EX_PANIC_PAYLOAD_TYPE`, `EX_PANIC_PAYLOAD_VALUE`,
   `EX_PANIC_PAYLOAD_FILE`, `EX_PANIC_PAYLOAD_LINE`,
   `EX_PANIC_PAYLOAD_DOWNS`.
6. **Effect / channel select:** `EX_WITH_HANDLER`, `EX_SELECT`.
7. **CPS transform (new since 2026-06-01):** `EX_CALLCC`,
   `EX_CPS_CONT_APP`. Both originate in the CPS pipeline
   (`docs/upcoming/cps-transform-plan.md`); the interpreter strategy
   for them should fall out of however that plan lands -- adding case
   arms ahead of it risks rework.

### Builtin natives registered only on the compiler side

The `EX_*` audit above counts expression kinds, but the parity gap also
shows up at the **builtin-native** layer: functions that exist as
compiler builtins with no corresponding registration in the turi
interpreter. These fail at runtime under `tur repl` and the WASM REPL
even when the source parses cleanly.

Known instances:

- **`rc/of`, `rc/clone`, `rc/drop`, `rc/strong-count`** -- registered
  only as compiler builtins (`src/compiler/builtins.c:140-146`). A grep
  of `src/turi/` returns zero references, so any program that touches
  `rc/*` runs fine under `tur build` / `tur run` but errors out under
  the interpreter and the WASM REPL. Doc snippets in
  `docs/guides/eval-api.md` that demonstrate `rc/of` are therefore
  misleading for sandbox/WASM readers.

TI8's `tools/check_turi_parity.py` should be extended to diff the
compiler's builtin-native registry against the interpreter's native
table (not just `EX_*` enums) so this class of gap is caught by the
same CI ratchet. Until then, treat any new `register_builtin` in
`src/compiler/builtins.c` without a matching `turi_register_native` as
a TI-gap to file.

### Typeclass-correctness churn since 2026-06-01 (audited 2026-06-10)

Between the 2026-06-01 draft and now, a stack of typeclass / poly-closure
fixes landed:

- Poly-closure typed-dispatch (all three layers): #293, #296, #297, #300
  -- inner-body intermediate-type erasure, capturing-closure return-type
  lowering, and `^fat`-param tyvar propagation through Direction 3 in
  `emit_expr.c`.
- Arrow `(->)` carrier-class routing (#318): `elab_call.c` refuses the
  int64-carrier `Arrow [(->)]` instance when the receiver's carrier
  type is float-class, falling back to the typed free `>>>` defn
  (`fn_type_has_float_carrier` at `src/compiler/elab_call.c:730`,
  gating `elab_user_method_instance_matches` at `:794`).
- `stdlib-hkt-consolidation` closed (new fixtures
  `hkt-stdlib-result-ok-biased`, `instance-head-hole-pair`,
  `errors/instance-head-two-holes`; Applicative `[(Result _ B)]` is
  deferred per that plan).
- `stdlib-type-erasure-cleanup` closed; Phase B1 spun into
  `stdlib-arrow-typeclass-reintroduction-plan`.

### TI0 audit result -- **clean, no gaps to file**

The interpreter consumes the *elaborated* `Expr` tree (entry at
`main.c:351` `elaborate_program`, then either compile or `turi_eval`).
That means the dispatch decisions made in `elab_call.c` -- the carrier
check, `prefer_method_dispatch`, the user-vs-stdlib `from_stdlib`
exclusion -- have **already happened** by the time turi sees the tree.
At evaluation time the interpreter just dereferences the elab-chosen
`e->as.dict_.instance` (`src/turi/eval.c:3327`). It cannot
re-introduce the #318 miscompile because it does not redo dispatch.

12 fixtures were run against `./build/tur run` on 2026-06-10 to confirm:

| Fixture                                | Result |
| -------------------------------------- | ------ |
| `arrow-compose-float`                  | PASS   |
| `arrow-instance-apply`                 | PASS   |
| `arrow-instance-arr-identity`          | PASS   |
| `arrow-instance-basic`                 | PASS   |
| `arrow-instance-choice`                | PASS   |
| `arrow-instance-stdlib-basic`          | PASS   |
| `fat-shim-void-ptr-arrow-compose`      | PASS   |
| `hkt-stdlib-result-ok-biased`          | PASS   |
| `instance-head-hole-pair`              | PASS   |
| `poly-closure-compose-float`           | PASS   |
| `poly-closure-result-tyvar-float`      | PASS   |
| `errors/instance-head-two-holes`       | PASS (diag matches, exit 1) |

All 12 are now on the TURI allowlist (commit at TI0 close); they
previously SKIPped because nobody had thought to add them. The
`errors/` fixture exercises the elaborator-error path manually --
`tests/run-turi.sh:330` excludes `tests/fixtures/errors/` from the
harness, so error-fixture coverage under turi remains a separate gap
(small follow-up: opt the `errors/` subtree into the turi harness, or
add a one-shot pass that runs them and only checks exit + diag).

**Residual TI0 work** -- the audit found one structural gap worth
filing as a small follow-up rather than blocking TI1:

- `tests/fixtures/errors/` is skipped wholesale by `run-turi.sh`. Worth
  opting in (separate flag, diag-only comparison) so the elaborator
  error path gets CI coverage under turi.

The carrier-class / free-defn-preference rule diff against the
interpreter is **moot**: there is no separate interpreter dispatch
path. TI9's parity matrix can list typeclass dispatch as `OK / OK`
truthfully.

### Test-harness gap

- `tests/run-turi.sh` ships an inline `TURI_FIXTURES_DEFAULT` string
  (around line 82-200) listing the 120 fixtures known to pass under
  turi. Everything else SKIPs with `(not in turi allowlist)`.
- Five fixtures carry `requires.compiled`; only one carries
  `requires.interp` (`tests/fixtures/tuple-arity-6`).
- The harness already supports `requires.dedicated-runner` and
  `requires.spices`; we can add a `requires.tur-only` symmetric to
  `requires.compiled`.

---

## Phase TI1 -- Quick wins (no design work)

Each item below is a 30-200 line case arm in `src/turi/eval.c` plus a
fixture.

### TI1.1 `EX_LETREC` -- **LANDED**

Mutually recursive bindings. The compiler emits a `letrec` form for
groups of `defn`/`defmacro` that reference each other. The interpreter
currently rejects them.

**Implementation (shipped):** Two-pass over the shared `as.let_` payload.
Pass 1 pre-binds each name to `turi_nil()` in one new frame; pass 2
evaluates each RHS in that frame and updates the binding in place. One
wrinkle the plan did not anticipate: the elaborator **hoists** a
recursive fn literal to a top-level static fn, so the binding's init
resolves (via `EX_VAR`) to a closure with `captured == NULL` registered
globally only under a mangled name, while the body's recursive reference
resolves to the lexical letrec name. The interpreter therefore binds a
shallow copy of any `captured == NULL` closure whose `captured` frame is
the shared letrec frame, so every sibling is reachable by its lexical
name at call time. See `src/turi/eval.c` `case EX_LETREC`.

**Fixture:** `tests/fixtures/letrec-basic/` (created -- self + mutual
recursion with an `expected.stdout`). The pre-existing `letrec-mutual`,
`letrec-self-recursive`, `letrec-shadows-outer`, and
`letrec-non-fn-no-self-ref` are codegen-snapshot fixtures (no
`expected.stdout`); all four now also evaluate correctly under
`tur --interpret`.

### TI1.2 `EX_SYM_LIT`

Interned symbol literal (`'foo`). Type `TY_SYM`. The compiled path
returns a runtime symbol handle.

**Implementation:** Add `TURI_SYM` to `TuriTag` in `src/turi/value.h`;
hold an interned id from a small per-env symbol table. `sym=?` already
works on `TURI_INT`; switch the comparison to use the new tag.

**Fixtures:** `tests/fixtures/sym-basic/`, `tests/fixtures/sym-eq/`.

### TI1.3 `EX_CONS_LIST`

Cons-list literal (e.g. variadic `& rest` argument bundling). Currently
the interpreter walks args one at a time; the compiler emits an explicit
`EX_CONS_LIST` for the variadic tail.

**Implementation:** Evaluate each element, allocate a chain of
`__tur_cons_cell { head; tail }` structs, return the head pointer
boxed as `TURI_INT` (matches the codegen ABI). Free at frame exit.

**Fixtures:** `tests/fixtures/variadic-basic/`,
`tests/fixtures/variadic-typed/`.

### TI1.4 `EX_SET_FIELD` -- **LANDED**

In-place struct field update (`(set! (.foo s) v)`). Compiled path
mutates the struct slot directly.

**Implementation (shipped):** Evaluate the receiver; deref a `TURI_REF`
mutable borrow and unwrap an `__rc` payload (`field[1]`) when
`receiver_is_rc`; store the evaluated RHS into `TuriStruct->fields[idx]`.
The compiler already rejects writes to immutable bindings during
elaboration, so the interpreter trusts the elaboration. See
`src/turi/eval.c` `case EX_SET_FIELD`.

**Fixture:** `tests/fixtures/struct-set-field/` (created; verified equal
under `tur run` and `tur --interpret`).

### TI1.5 `EX_HANDLER_LIT` + `EX_COMPOSE_HANDLERS`

Handler-record literal and the operator that splices two handlers
together. The interpreter currently models handlers as ad-hoc closures
inside `EX_WITH_HANDLER`; the explicit handler-literal node is a newer
form.

**Implementation:** Add a `TURI_HANDLER` value (or reuse `TURI_STRUCT`
with a sentinel struct type). For `EX_COMPOSE_HANDLERS`, build a list
of `(op -> closure)` pairs whose later-bound entries override earlier
ones.

**Fixtures:** `tests/fixtures/handler-lit/`,
`tests/fixtures/handler-compose/`.

### TI1.6 `EX_OR_ELSE`

STM `or-else` short-circuit. Even outside the STM context, the compiler
uses `or-else` for backtracking-style fallbacks.

**Implementation:** Evaluate the first branch; if it returns the
`Retry` sentinel (a tagged value the STM layer introduces in TI4), run
the second.  Outside an STM block, the first branch can't retry, so
this degenerates to "evaluate first; if error, evaluate second" -- pin
the exact semantics in a fixture before writing the case arm.

### TI1.7 `EX_CHECK`

Compile-time `(check expr)` contract assertion that the compiler
lowers to a runtime check when `--no-contracts` is off (see
`error-handling-deferred-plan.md` Phase C2).

**Implementation:** Evaluate the predicate; if `false`, call
`turi_error` with the source-span message.

### Doc / harness updates

- Add a `requires.tur-only` marker. Update `tests/run-turi.sh` to
  PASS-skip fixtures bearing it (mirror of `requires.compiled`).
  **DONE** -- `tests/run-turi.sh` now PASS-skips `requires.tur-only`
  fixtures alongside `requires.compiled`.
- After TI1 lands, sweep the existing fixtures and add the
  newly-passing ones to the allowlist. Target: >=250 fixtures.
  `letrec-basic` and `struct-set-field` added; broad sweep is **blocked**
  on the harness-wiring bug below.

### BLOCKER discovered during TI1 -- the harness does not interpret

`tests/run-turi.sh` (and the fixture-running parts of
`tests/run-flags.sh`) invoke `tur run <file>`, which **compiles and runs
a native binary**, not the `turi` interpreter (`tur --interpret` /
`tur eval --file`). The allowlist has therefore never exercised
`src/turi/eval.c`: allowlisted fixtures pass via codegen. Running the
current allowlist through the real interpreter surfaces ~31 hidden
failures (including four fixtures the TI0 audit "verified" via
`./build/tur run`). The TI1 interpreter work here was consequently
verified directly with `tur --interpret`, not through the harness.

Full write-up, repro, and the recommended (cascading) fix:
[docs/reported/turi-harness-compiles-instead-of-interpreting.md](../../reported/turi-harness-compiles-instead-of-interpreting.md).
The wiring flip belongs with the TI8 triage (it turns CI red until the
~31 fixtures are fixed or carved out) and is intentionally **not**
bundled into TI1.

### TI1 items still open

`EX_SYM_LIT`, `EX_CONS_LIST`, `EX_HANDLER_LIT`/`EX_COMPOSE_HANDLERS`,
`EX_OR_ELSE`, and `EX_CHECK` remain unimplemented. They are less
self-contained than the plan's "quick wins" framing implies:

- The existing `sym-*` and `variadic-*` fixtures pair `EX_SYM_LIT` /
  `EX_CONS_LIST` with **user inline-C** (`__tur_sym` field reads,
  `__tur_cons_cell` walks), which is a permanent interpreter carve-out
  (TI7). Landing these usefully requires native `sym=?`/`sym->str` and
  `cons-head`/`cons-tail` overrides, not just the literal case arm.
- `EX_OR_ELSE` and `EX_CHECK` are only meaningful inside an STM
  transaction (TI4); the plan itself says to "pin the exact semantics
  in a fixture before writing the case arm." Best done alongside TI4.
- `EX_HANDLER_LIT`/`EX_COMPOSE_HANDLERS` overlap with the handler-record
  work in TI6.

---

## Phase TI2 -- Generators

The compiler emits `EX_GEN` (generator constructor), `EX_GEN_NEXT`,
`EX_GEN_DONE`, and `EX_YIELD` for `(gen ...)` and `for*`-style loops.
Today these are unhandled.

### Implementation

Two viable strategies:

1. **Fiber-backed.** Use the `ucontext` machinery already in
   `src/turi/fiber.c` (Phase S7) to run each generator body on its own
   stack; `yield` swaps back to the caller, `next` swaps in. Cheap to
   write because the swap primitives exist; cost is per-generator
   stack allocation.
2. **CPS-rewrite at elaboration.** The compiler already has a CPS
   pipeline (`docs/upcoming/cps-transform-plan.md`). Reuse it to flatten
   the generator body into an explicit state machine, then evaluate
   that state machine in the interpreter without stack juggling.

**Recommendation:** (1). Strategy 2 requires CPS-transform to land
first; the fiber strategy is self-contained.

### Tests

- `tests/fixtures/gen-basic/` -- yields 1..3, sums.
- `tests/fixtures/gen-done/` -- finite generator, `gen/done?` is true.
- `tests/fixtures/gen-nested/` -- generator yielding generators.
- `tests/fixtures/gen-defer/` -- defer thunks fire when the generator
  is dropped without being exhausted.

### Doc

Update `docs/guides/generators-guide.md` to remove any "interpreter
support TBD" caveats.

---

## Phase TI3 -- Delimited control parity

`shift` / `reset` / `shift0` / their cloneable variants / serial
variants. The interpreter today only supports the **basic effect**
form via `EX_PERFORM` (already handled) and chokes on the raw
delimited-control nodes.

### Key finding: Turmeric `shift`/`shift0` are ABORTIVE

The plan's original design (capture body-up-to-reset as a
`TuriEffectCont`, swap to the reset handler) assumed full
continuation-passing semantics for `shift`. The audit of the compiled
path corrected this: `EX_SHIFT` / `EX_SHIFT0` are **abortive**.
`(shift f body)` lowers to "evaluate `body` to `v`, compute `f(v)`,
abort to the nearest enclosing `reset` whose value becomes `f(v)`" --
the captured sub-continuation is *never resumed* (the emitted runtime
body is `__dk_abort_body`, which ignores the captured slice; see
`src/runtime/cps_prompt.c`). `shift0` differs only in prompt
re-installation on resume, which is unobservable when the continuation
is discarded. `tests/fixtures/continuation-substrate/` asserts exactly
this (`t-nested-reset=11` proves the `(+ 10 ...)` frame is discarded).

Because the continuation is discarded, the abortive operators need a
plain `setjmp`/`longjmp` prompt boundary -- **no fiber capture**. Only
the context-*capturing* variants (`serial-shift` / `cloneable-shift`,
which hand a resumable `k` to `f`) need `TuriEffectCont`-style capture.

### TI3.1 -- abortive base + prompt boundaries -- **LANDED**

`src/turi/eval.c` now handles:

- `EX_RESET`, `EX_SHIFT`, `EX_SHIFT0` -- abortive, via a thread-local
  `TuriResetBoundary` `setjmp`/`longjmp` stack (`eval_reset_boundary` /
  `eval_abortive_shift`). The boundary snapshots `eval_depth`,
  `handler_stack`, and `defer_stack` and restores them after a
  `longjmp` unwinds the intervening eval frames.
- `EX_SERIAL_RESET`, `EX_CLONEABLE_RESET` -- establish a prompt
  boundary so the no-shift passthrough case evaluates (e.g.
  `(serial-reset 42) => 42`). (The previous `EX_SERIAL_RESET` "Phase 21
  not yet implemented" error arm is removed.)

Verified under `tur --interpret` (the harness still compiles -- see the
TI1 blocker): `continuation-substrate` (43/107/11/6/42/123/16),
`shift-result-typing` (1/0/42), `shift0-result-typing` (1/0),
`serial-reset-basic` (`result: 42`). All four added to the
`tests/run-turi.sh` allowlist (TI3 block).

### TI3.2 -- context-capturing shift -- **carved out (follow-up)**

`serial-shift` and `cloneable-shift` (and multi-shot cloneable resume)
remain unimplemented in the interpreter and error cleanly under
`tur --interpret`. They need genuine continuation capture (reuse
`TuriEffectCont` for one-shot serial; multi-shot cloneable needs either
fiber-stack cloning or a heap-reified `DK` chain). Full write-up, repro,
and recommended fix directions:
[docs/reported/turi-capturing-shift-unimplemented.md](../../reported/turi-capturing-shift-unimplemented.md).

`call/cc` / `escape` (`EX_CALLCC`) used by `cont-flavors`, `callcc-*`,
and `escape-*` are tracked separately under the CPS-transform category
(Risks #5 / Sequencing) and are out of TI3 scope.

### Doc

Cross-link from `docs/guides/delimited-control-operators-guide.md`.

---

## Phase TI4 -- STM -- **LANDED (interpreter)**

`EX_STM`, `EX_ATOMICALLY`, `EX_RETRY`, `EX_CHECK`, `EX_OR_ELSE`, and the
`EX_TVAR_*` family are now handled in `src/turi/eval.c`.

### Implementation (shipped)

The interpreter is single-threaded, so STM collapses to a write-log
transaction model (no real concurrency, matching the plan):

- A `TVar` is a heap cell `{ value; version }`. Values are int64 boxed
  as `ptr<void>` (the compiled ABI stores `(void*)(intptr_t)init`),
  represented as `TURI_INT`.
- `EX_ATOMICALLY` (`eval_atomically`) runs the `EX_STM` body against a
  fresh write-log transaction (thread-local `g_stm_tx`). Reads see
  buffered writes (read-your-writes); on normal completion the log is
  committed (writes applied, versions bumped). With no concurrent
  writer, read-set validation always succeeds, so commit never fails.
- `EX_TVAR_NEW`/`READ`/`WRITE`/`SWAP`/`CAS`/`MODIFY` operate on the
  active transaction's write log. `cas` returns `bool`; `swap`/`modify`
  return the old value.
- `EX_CHECK`/`EX_RETRY` request a retry. A serial retry can never make
  progress (nothing else mutates the TVars), so an unguarded retry that
  reaches `atomically` errors out instead of hanging (the compiled path
  blocks on a condvar forever). `EX_OR_ELSE` clears a retry from stm1
  and runs stm2 in the same transaction, so well-formed `or-else` never
  reaches that check.

Verified end-to-end under `tur --interpret` via the **`tur_eval_stm`**
ctest target (`tests/turi/eval-stm.sh` + `eval-stm.tur`): tvar new/
write/read/swap/cas, atomically/stm, check, and or-else (both the
retry-fallback and the success-no-fallback paths).

### Discovered + fixed: compiled-path cas/swap/modify

Authoring the fixture surfaced a compiled-side bug: `tvar/cas` and
`tvar/swap` **failed to link** (codegen emitted calls the runtime never
defined) and `tvar/modify` codegen was a **no-op stub**. **Now fixed**
(report executed): `tur_tvar_cas`/`tur_tvar_swap` are emitted in the
runtime preamble, `tvar/modify` is lowered in the elaborator to
`(let [g tv] (tvar/swap g (f (tvar/read g))))` so it reuses the normal
call dispatch on both backends, and the runtime `tur_tvar_modify` writes
`fn(old)`. `tests/fixtures/stm-cas/` guards the compiled path; all 73
`expected.c` snapshots were regenerated for the two new runtime
functions. See
[docs/reported/stm-tvar-cas-swap-modify-compiled-path-broken.md](../../reported/stm-tvar-cas-swap-modify-compiled-path-broken.md).
A separate compiled-`or-else` bug (branches emit as no-op stubs) was
found while validating and filed at
[docs/reported/stm-or-else-compiled-branches-are-noop-stubs.md](../../reported/stm-or-else-compiled-branches-are-noop-stubs.md);
it is **not** fixed here.

### Tests / harness note

The `tur run`-based harnesses cannot exercise this work (the
harness-compiles blocker, and cas/swap don't link), so coverage lives
in the dedicated `tur_eval_stm` ctest target rather than the
`run-turi.sh` allowlist. When the harness is flipped to true
interpretation (TI8), STM fixtures can move onto the allowlist; the
broken compiled cas/swap/modify must be fixed first (see report) before
they can be cross-checked on both paths.

### Doc

Update `docs/guides/effects-system-guide.md` STM section to clarify
"turi runs STM transactions serially; tur uses real lock-free
versioning."

---

## Phase TI5 -- Panic payloads + `catch-panic-of` -- **LANDED (interpreter)**

**Depends on:** `error-handling-deferred-plan.md` Phase R2 -- **landed**
(commit `0de95bcc`, "Phase R2 + R6c: catch-unwind and panic handling on
compiled path"). TI5 is no longer blocked; it ran in parallel with
TI2-TI4.

`EX_CATCH_PANIC_OF` and the `EX_PANIC_PAYLOAD_{TYPE,VALUE,FILE,LINE,DOWNS}`
family are now handled in `src/turi/eval.c`.

### Implementation (shipped)

The interpreter's `catch_jmp` setjmp boundary now carries a typed panic
payload. Four fields were added to `TuriEnv` (`src/turi/env.h`):
`catch_panic_type` (a `TypeKind` stored as int), `catch_panic_value`
(the panicked `TuriValue`), `catch_panic_file`, and `catch_panic_line`.

- **Panic raise.** `turi_runtime_panic` (plain `(panic msg)` and native
  `result-must`/`option-must`/...) stamps a `:cstr` payload whose value is
  the message, so `catch-panic-of :cstr` matches a string panic.
  `EX_PANIC_WITH` evaluates its operand and stamps the operand's
  `type.kind` + value + source line.
- **`EX_CATCH_PANIC_OF`.** Installs its own `setjmp` boundary. On a caught
  panic it compares `catch_panic_type` against the requested `type_kind`:
  on a match it consumes the panic and returns `(err payload)`; on a
  mismatch it re-raises to the next outer boundary (`longjmp(prev_jmp)`),
  or prints `panic at` + fires defers + exits if it is outermost -- mirroring
  the compiled `tur_catch_panic_of` re-panic.
- **Accessors.** A caught result's err slot boxes a heap `TuriPanicPayload`
  (`{ type_tag; value; file; line }`); `EX_PANIC_PAYLOAD_*` cast that
  pointer back and read the field. `_DOWNS` returns the value only when the
  tag matches its `target_type`, else nil. `catch-unwind` was refactored to
  share the same `turi_ok_result_box` / `turi_err_result_box` helpers, so
  its err slot now carries the payload too.

### Tests

- `panic-catch-panic-of` -- plain (cstr) panic: `:cstr` matches, `:int`
  re-raises to an outer `catch-unwind`. **Now passes under `tur --interpret`**
  (previously hit the unhandled-kind default).
- `panic-with-catch-of` (new) -- typed `panic-with` int payload: `:int`
  matches, `:cstr` re-raises. Verified equal under `tur --interpret` and
  `tur run`.

Both added to the `run-turi.sh` allowlist (TI5 block).

**Reachability note.** The `panic-payload-*` accessors are implemented and
correct, but a *pure-turi* program cannot obtain the payload handle without
the inline-C `result-panic` extractor (`stdlib/panic.tur`) -- `catch-panic-of`
returns the `:int` Result box, and `err-val` does not typecheck against it.
So the turi-testable surface is the `catch-panic-of` type-filtering path; the
accessors mainly serve to close the `EX_*` parity gap and would work if reached
via a native handle.

---

## Phase TI6 -- `EX_WITH_HANDLER` and `EX_SELECT`

### `EX_WITH_HANDLER` (+ `EX_HANDLER_LIT` / `EX_COMPOSE_HANDLERS`) -- **LANDED**

First-class handler *values* are now interpreted. The three nodes the
elaborator emits for the FH (first-class handler) surface are all handled in
`src/turi/eval.c`:

- `EX_HANDLER_LIT` -- `(handler (E [params] k) body)` builds a detached
  dispatch table. A new `TURI_HANDLER` value (`TuriHandlerVal` in `eval.c`,
  tag in `src/turi/value.h`) borrows pointers to the arena-allocated
  `HandleCase`s; no body is attached.
- `EX_COMPOSE_HANDLERS` -- `(compose-handlers h1 h2)` concatenates the two
  tables (h1's cases first -- h1 outer per FH0.1). The elaborator already
  rejects overlapping effect sets (`TUR-E0251`), so first-match dispatch
  order across the two is unobservable.
- `EX_WITH_HANDLER` -- `(with-handler hv body)` materialises a contiguous
  `HandleCase` array + a synthesised `HandleExpr` and reuses the existing
  `eval_handle` fiber machinery. Stack allocation is safe because
  `eval_handle` runs the body (and every resume) to completion before
  returning, and continuations never escape it.

Note `(with-handler body cases...)` with any arity != 3 is the T25
inline-handle *sugar* (`elab_call.c:1112-1114`) and lowers to `EX_HANDLE`,
which the interpreter already handled.

**Fixtures:** `tests/fixtures/with-handler-value/` (single handler value),
`tests/fixtures/fh-compose-handlers/` (compose over disjoint effects). Both
verified equal under `tur --interpret` and `tur run`; both added to the
`run-turi.sh` allowlist (TI6 block).

### `EX_SELECT` -- **carved out (follow-up)**

Channel `select` over multiple receive/send ops stays unimplemented in the
interpreter. Turmeric channels have no native representation in `turi`
(they are inline-C `pthread` ring buffers), and **every** existing `select-*`
fixture defines its channel ops as user inline-C -- a permanent TI7
carve-out -- so a `turi_select` case arm would have nothing to select over.
A dedicated `case EX_SELECT` now returns a clean
"not supported in interpreter mode" error instead of the generic
unhandled-kind default. Implementing it for real means adding a native
channel layer (opaque `TURI_CHANNEL` + `chan-new`/`send`/`recv` natives +
a fiber-parking `turi_select`), plus native (non-inline-C) channel fixtures
to test against. Full write-up:
[docs/reported/turi-select-needs-channel-primitives.md](../../reported/turi-select-needs-channel-primitives.md).

---

## Phase TI7 -- Documented escape hatches

Some kinds will remain unimplemented by choice. This phase **codifies**
those choices rather than implementing them.

### Inline-C (`EX_INLINE_C`)

Permanent carve-out. The existing native-override path
(`eval.c:2390-2419`) lets stdlib inline-C functions register a C
implementation that turi calls; `try_exec_simple_inline_c` covers the
common malloc/field-load/field-store shapes for stdlib structs. User
inline-C falls through to the existing error message.

**Doc:** Add a "Why inline-C is not interpreted" subsection to
`docs/guides/eval-api.md`.

### WASM async

Permanent carve-out per `src/turi/fiber.c:172-288`. Document in
`docs/guides/eval-api.md` and the WASM section of
`docs/guides/async-await-guide.md`.

---

## Phase TI8 -- Harness flip: allowlist → denylist -- **PARTIAL (ratchet + harness now interprets)**

### TI8.a -- CI ratchet + harness genuinely interprets -- **LANDED**

The foundational correctness fix and the CI ratchet shipped:

- **`tools/check_turi_parity.py`** (new) diffs the `EX_*` enumerators in
  `src/compiler/expr.h` against the `case EX_*:` arms in `src/turi/eval.c`.
  Any unhandled kind that is not listed in **`docs/turi-carve-out.txt`** (new,
  6 entries with rationale) fails the check; a stale carve-out (a kind that is
  actually handled, or a nonexistent kind) also fails, keeping the list honest.
  Wired into `tests/run.sh` as a pre-test gate (opt out with
  `TUR_SKIP_PARITY_CHECK=1`). Current state: **109/115 handled, 6 carved out,
  0 gaps.**
- **`tests/run-turi.sh` now runs `tur --interpret`**, not `tur run`. This
  resolves the blocker
  ([turi-harness-compiles-instead-of-interpreting.md](../../reported/turi-harness-compiles-instead-of-interpreting.md)):
  the allowlist finally exercises `src/turi/eval.c`. Reconciling to true
  interpretation removed **31 false-green entries** (catalogued in
  [turi-harness-flip-reconciliation.md](../../reported/turi-harness-flip-reconciliation.md));
  the harness is green at **122 passed, 0 failed**. The `requires.tur-only`
  marker (from TI1) is honored as the symmetric skip to `requires.compiled`.

### TI8.b -- Full allowlist → denylist flip -- **IN PROGRESS (defmodule defect fixed)**

The flip itself (delete the allowlist; default to run-everything-minus-markers)
is **not** done -- the blast radius is large and includes silent miscompiles
that must be fixed or carved first. Measured on 2026-06-11: under `--interpret`,
**637 pass / 933 fail / 92 skip** across all fixtures.

**Landed in TI8.b so far -- the `defmodule` concatenation defect:** the 46
`only one defmodule is allowed per file` failures were a real interpreter bug.
`cmd_eval` preloaded `macros.tur` (which carries `(defmodule tur/macros ...)`)
by **concatenating** its source into the single `<eval>` blob (`file_id 0`), so
any user fixture with its own defmodule collided. Fixed by preloading
`macros.tur` via a `(load ...)` form (which assigns it a distinct `file_id`, so
the per-file `has_defmodule` reset fires). 23 module/defmodule fixtures now pass
and were added to the allowlist (harness: **145 passed, 0 failed**); post-fix
probe **660 / 910 / 92**. See the reconciliation report for the de-risked
roadmap on the next-largest bucket (the typed-stdlib preload, which needs the
`(load ...)` mechanism + dropping benchmark-stub conflicts + a perf
consideration).

Remaining buckets in the 910: typed-stdlib preload (missing natives/struct
types), typeclass-registration gaps during stdlib load, move/linearity-checker
divergence, inline-C carve-outs, and silent wrong-value miscompiles. Each must
be fixed in `src/turi/eval.c` or tagged `requires.tur-only`/`requires.compiled`
before the flip lands green.

Remaining steps when the flip is tackled:

1. Delete `TURI_FIXTURES_DEFAULT` from `tests/run-turi.sh`.
2. Default to "run every fixture" minus `requires.{compiled,tur-only,
   dedicated-runner,spices}`.
3. Retire the `KB-001` allowlist-gap workaround comment.
4. Also flip `tests/run-flags.sh`'s three `tur run` assertions (`:345`,
   `:355`, `:408`) to `--interpret`.

---

## Phase TI9 -- Parity matrix guide

Create `docs/guides/turi-parity-guide.md`. Sections:

1. **What turi is** -- one-paragraph definition; cross-link to
   `docs/guides/eval-api.md` and `docs/guides/repl.md`.
2. **Parity matrix** -- a Markdown table, one row per language
   feature group:
   - Pattern matching, typeclasses, modules, macros, currying,
     borrows, rc/weak/box, structs/ADTs, GADTs, HKT, effects,
     handlers, continuations, generators, STM, channels, async,
     dynamic vars, panic/catch, sized primitives, sweet-exp/neoteric.
   - Columns: "tur (compiled)", "turi (interp)", "Notes".
   - Each cell is `OK`, `partial`, `none`, or `n/a`.
3. **Documented carve-outs** -- one paragraph per carve-out (inline-C,
   WASM async, anything from TI7).
4. **Performance note** -- "turi is a tree walker; expect 10-100x
   compiled-code slowdown."
5. **How to ask 'does feature X work?'** -- show `tools/check_turi_parity.py`
   and the parity matrix above.

Add to `docs/guides/README.md` and the per-module doc index.

---

## Sequencing & dependencies

```
TI0 (typeclass audit -- DONE 2026-06-10) ─► TI1 (quick wins) ──► TI8 (harness flip; >=80%)
                                                  │                  │
                                  TI2 (generators) ┤                  ├─► TI9 (parity matrix doc)
                                  TI3 (delim control) ─┤              │
                                  TI4 (STM) ───────────┤              │
                                  TI5 (panic payloads) ┤              │
                                  TI6 (with-handler + select) ────────┘

TI7 (carve-outs) ── independent; can land any time
```

TI5 was blocked on `error-handling-deferred-plan.md` R2 in the original
draft; that has since landed (`0de95bcc`) and TI5 is now free-floating.

Suggested order: **TI0 (done) → TI1 → TI7 → TI8 (partial; just the
marker + script) → TI2 → TI3 → TI4 → TI6 → TI5 → TI8 (full flip) →
TI9**.

TI7 + the partial TI8 ship first so we have a CI ratchet preventing
the gap from growing while the bigger phases land.

---

## Risks

1. **Generator / shift-reset stack juggling.** `ucontext` is
   deprecated on POSIX and stubbed on macOS/AArch64; the existing
   fiber code already works around this with the platform `#pragma`s
   visible at `src/turi/eval.c:44-52` and `:155-188`. Expect the same friction
   for TI2 and TI3. Mitigate by reusing the fiber primitives rather
   than rolling new ones.
2. **STM semantics drift.** A single-threaded STM is sound for testing
   but can silently miss races the compiled path would expose.
   Document the divergence in the parity matrix; do not try to
   simulate concurrency in the interpreter.
3. **Allowlist → denylist flip flushes hidden failures.** The current
   ~900 SKIPped fixtures probably include some that genuinely should
   not run under turi (e.g., inline-C-heavy fixtures). Triage in TI8
   by running the harness with a *temporary* `set -e`-free pass that
   collects every failure, then bulk-tag the non-portable ones with
   `requires.tur-only`. Budget a day for this triage.
4. **Symbol-table memory.** The interpreter intentionally never frees
   its symbol table (see CLAUDE.md "Leak detection (ASan/LSan)
   policy"). Make sure TI1.2 (`EX_SYM_LIT`) reuses the same lifetime
   convention; LSan suppressions live in `tests/run-turi.sh` already
   (`ASAN_OPTIONS=detect_leaks=0`).
5. **CPS-transform interplay.** If `docs/upcoming/cps-transform-plan.md`
   lands first and changes how generators / shift-reset are
   represented, TI2-TI3 should rebase on top of it rather than
   building a parallel implementation. Coordinate before starting TI2.

---

## Open Questions

1. **Should `requires.tur-only` exist?** Symmetric to
   `requires.compiled`, it makes the harness denylist clean. The
   alternative is to keep the allowlist and bulk-expand it; cheaper
   short-term but loses the "new fixtures default to turi" property.
   Plan above commits to `requires.tur-only`.
2. **Should the parity script live in CMake or `tests/run.sh`?**
   CMake makes it run on every build; `tests/run.sh` only on test
   runs. Initial recommendation: `tests/run.sh` (cheap to run, fails
   loudly, doesn't slow `tur build`).
3. **Sandbox capabilities and parity.** The eval API exposes
   `TURI_CAP_INLINE_C` and `TURI_CAP_ASYNC` etc. as separate caps.
   Should the parity matrix have a third column "sandbox-default"
   showing what is on / off when the sandbox caps are unset? Probably
   yes; defer the column to a TI9 follow-up if the table is already
   too wide.
4. **Replacing the archived plan.** Should the May-2026
   `docs/archive/interpreter-features-plan.md` be moved to
   `docs/archive/history/` (the dead-archive subtree per the
   `churn-docs` convention) once this plan supersedes it? The
   churn-docs skill handles this; do it at TI9 close.

---

## See Also

- [docs/archive/interpreter-features-plan.md](../archive/interpreter-features-plan.md)
  -- prior pass at this work (May 2026); much of Phase S4 shipped from
  there.
- [docs/guides/eval-api.md](../guides/eval-api.md) -- libturi public C
  API.
- [docs/guides/repl.md](../guides/repl.md) -- REPL surface that runs on
  turi.
- [docs/upcoming/cps-transform-plan.md](cps-transform-plan.md) -- may
  reshape how TI2-TI3 are implemented.
- [docs/upcoming/error-handling-deferred-plan.md](error-handling-deferred-plan.md)
  -- R2 (`catch-unwind`) blocks TI5.
- [src/turi/eval.c](../../src/turi/eval.c) -- the file that grows in
  this plan.
- [tests/run-turi.sh](../../tests/run-turi.sh) -- the harness that
  flips from allowlist to denylist in TI8.
