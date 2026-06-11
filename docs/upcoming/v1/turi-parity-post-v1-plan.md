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

### Implementation

The interpreter has `ucontext` swaps already; the delimited-control
ops can be expressed in terms of `swapcontext` plus a per-`reset`
boundary record. Reuse `TuriEffectCont` for captured continuations:

- `EX_RESET`: push a new reset boundary record, run body, pop on
  return.
- `EX_SHIFT`: walk to nearest reset, capture body-up-to-reset as a
  `TuriEffectCont`, swap to the reset handler with the captured
  continuation as its argument.
- `EX_SHIFT0`: same but pop the boundary before invoking.
- **Cloneable**: wrap the captured stack in a refcounted copy that can
  be invoked more than once. Cost is duplicating the saved
  `ucontext_t` + register state.
- **Serial**: a single-shot variant; capture but mark "consumed" once
  invoked.

### Tests

Reuse the existing compiled-side fixtures (`delimited-control-*`) and
add them to the turi allowlist.

### Doc

Cross-link from `docs/guides/delimited-control-operators-guide.md`.

---

## Phase TI4 -- STM

`EX_STM`, `EX_ATOMICALLY`, `EX_RETRY`, and the `EX_TVAR_*` family.

### Implementation

The interpreter is single-threaded, so the STM semantics collapse to
a transaction-log model:

- A `TVar` is a heap-allocated `{ id; value; version }`.
- `EX_ATOMICALLY` opens a fresh log, runs the body. On `commit`, walks
  the log and bumps versions; on `EX_RETRY`, discards the log and runs
  the body again (or yields control if combined with channels --
  defer that complexity for now).
- `EX_OR_ELSE` becomes meaningful here (see TI1.6): try the first
  branch; if it retries, run the second.
- `EX_TVAR_CAS`: compare the log's recorded version to the live one;
  fail the transaction if mismatched.

A single-threaded STM is functionally complete for testing; the
multi-threaded story stays in the compiled path.

### Tests

`tests/fixtures/stm-basic/`, `stm-or-else/`, `stm-retry/`,
`stm-tvar-cas/`. Most already exist on the compiled side and just
need to enter the allowlist.

### Doc

Update `docs/guides/effects-system-guide.md` STM section to clarify
"turi runs STM transactions serially; tur uses real lock-free
versioning."

---

## Phase TI5 -- Panic payloads + `catch-panic-of`

**Depends on:** `error-handling-deferred-plan.md` Phase R2 -- **landed**
(commit `0de95bcc`, "Phase R2 + R6c: catch-unwind and panic handling on
compiled path"). TI5 is no longer blocked; it can run in parallel with
TI2-TI4.

### Implementation

The interpreter already has a `catch_jmp` setjmp boundary in
`src/turi/env.h:154`. Extend it to carry a `tur_panic_payload`
(matching the compiled-path struct at
`src/runtime/runtime.h:261`). The accessors
(`EX_PANIC_PAYLOAD_TYPE`, `_VALUE`, `_FILE`, `_LINE`, `_DOWNS`) read
fields off that payload. `EX_CATCH_PANIC_OF` does the type-tag
downcast before deciding whether to catch or re-raise.

### Tests

Mirror the R2 fixtures from `error-handling-deferred-plan.md` under
the turi harness.

---

## Phase TI6 -- `EX_WITH_HANDLER` and `EX_SELECT`

### `EX_WITH_HANDLER`

The interpreter handles `(handle ...)` via an older path; the
compiler now emits `EX_WITH_HANDLER` for `with-handler` (the
handler-record-driven form). Bridge the two by lowering
`EX_WITH_HANDLER` to the same internal "push handler, run body, pop
handler" routine `EX_HANDLE` already uses.

### `EX_SELECT`

Channel `select` over multiple receive/send ops. Today the
interpreter only handles single-channel `recv`/`send`. Add a
`turi_select` helper that polls each branch in declaration order
(non-blocking under the single-threaded model; if no branch is ready
and a `:default` arm exists, run it; otherwise park the fiber until
any channel becomes ready -- reuse the fiber scheduler from
`src/turi/fiber.c`).

### Tests

`tests/fixtures/channel-select-default/`, `channel-select-park/`.

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

## Phase TI8 -- Harness flip: allowlist → denylist

Once TI1-TI6 land, almost every fixture should run under turi.

### Implementation

1. Delete the `TURI_FIXTURES_DEFAULT` allowlist from
   `tests/run-turi.sh` (lines 82-211 as of 2026-06-10).
2. Default to "run every fixture under tests/fixtures/" minus those
   carrying:
   - `requires.compiled`
   - `requires.tur-only` (new marker from TI1)
   - `requires.dedicated-runner`
   - `requires.spices` (when `../turmeric-spices` is absent)
3. The `KB-001` known-bug ("allowlist gaps go unnoticed") becomes
   moot; remove the workaround comment.
4. Add a flagging script `tools/check_turi_parity.py` that:
   - Greps `src/compiler/expr.h` for `EX_*` enumerators.
   - Greps `src/turi/eval.c` for `case EX_*` arms.
   - Fails the build if any `EX_*` in (1) but not (2) is **not** in a
     known carve-out allowlist (`docs/turi-carve-out.txt` -- a short
     plain-text file).
5. Wire the script into `tests/run.sh` (or the CMake build) as a
   pre-test check.

### Tests

The harness flip itself is the test. CI run of `tests/run-turi.sh`
should land near-zero `SKIP` lines that aren't `requires.compiled`.

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
