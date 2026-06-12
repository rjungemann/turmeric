# turi Interpreter Gap-Closure Plan (TI8.b execution detail)

> **Status:** Draft Plan
> **Last Updated:** 2026-06-11
> **Type:** Interpreter / Test Infra
> **Scope:** post-v1 -- the remaining work to flip `tests/run-turi.sh` from a
> curated allowlist to a denylist (every fixture runs under `tur --interpret`
> minus documented carve-outs).
> **Parent:** [turi-parity-post-v1-plan.md](turi-parity-post-v1-plan.md) Phase TI8.b
> **Companion report:** [docs/reported/turi-harness-flip-reconciliation.md](../../reported/turi-harness-flip-reconciliation.md)

---

## Overview

TI8.a (CI ratchet + harness genuinely interprets + honest 145-fixture allowlist)
and the TI8.b `defmodule` concatenation fix have landed. What remains is the
*body* of TI8.b: drive the ~910 interpreter failures to zero -- either by fixing
the interpreter, or by carving the fixture out with a marker and a reason -- so
the allowlist can be deleted and the harness defaulted to "run everything minus
markers."

This plan exists because that work is **multi-session and spans several distinct
root causes** (some are missing-feature gaps, some are semantic divergences, a
few are silent miscompiles). Doing it as one blind flip turns CI red; doing it
without a map risks fixing low-value fixtures while the scary ones (silent
wrong-output) sit unexamined. The plan below sequences the work by leverage and
by severity, and pins a hard fix-vs-carve decision rule so progress is
mechanical, not improvised.

---

## Current state (measured 2026-06-11)

Full probe: run every `tests/fixtures/*` under `tur --interpret`, minus those
already carrying `requires.{compiled,tur-only,dedicated-runner,spices,tsan}`:

```
post-defmodule-fix:  pass = 660   fail = 910   skip = 92
post-W1:             pass = 695   fail = 875   skip = 92   (+35)
```

**Harness state (the live metric): 912 passed, 0 failed.** Progression:
181 (post-defmodule) -> 463 (W3 wired `errors/*`, +282) -> **912** (the
bulk-add of every auto-verified-passing non-inline-C fixture, +449). The harness
summary now separates the work cleanly:

```
912 passed, 0 failed, 665 skipped
  405 inline-c carve-outs   (TI7, permanent -- W2)
  260 non-inline-C not yet on the allowlist  (the W5 triage surface)
```

The **260** is the real remaining gap: ~244 genuine pure-turi failures (W1b
native-shim cluster + W4 silent miscompiles + an HKT/existential/continuation
tail) plus a few container/edge dirs. Everything that passes under `--interpret`
is now on the allowlist, so the allowlist == "everything that works," and a W5
flip to denylist only needs the 260 fixed-or-carved.

> The bucket tables below describe the **pre-W1 (910-failure)** snapshot, which
> is still the right map for the remaining failure work.

EX_* expression-kind parity (the `check_turi_parity.py` ratchet):
**109 / 115 handled, 6 carved out, 0 gaps.** So the remaining failures are
*not* about unhandled expression kinds -- they are stdlib/native/semantic gaps.

The 910 failures split cleanly along the single most important axis:

| Class | Count | Disposition |
| --- | ---: | --- |
| **inline-C-bound** (fixture body has a ` ```c ` block) | **377** | carve-out candidates (`requires.tur-only`) -- the interpreter never runs user inline-C (TI7) |
| **pure-turi** (no inline-C in the fixture) | **533** | genuinely fixable interpreter gaps |

The 533 pure-turi failures bucket (by first diagnostic) into:

| Bucket | ~Count | Root cause |
| --- | ---: | --- |
| Missing typed-stdlib prelude | ~93+ | interpreter does not preload `list/map/vec/option/result/...` + typeclass stubs, so `Cons`/`Option`/`map-new`/`Eq`/`Clone`/... are unbound (see W1) |
| Empty-stderr (silent) | ~155 | mix of silent miscompiles (rc=0, wrong stdout), aborts, and errors printed to stdout; **must be triaged individually** (see W4) |
| Move / linearity divergence | ~33 | interpreter runs the affine/linear checker and disagrees with the compiled path (`linear value dropped`, `use-after-move`, `linear used after consume`) |
| `if condition must be bool, got int` | ~5-13 | a predicate native's elaborated return type defaults to `:int` because its module/stub is not loaded (overlaps W1) |
| Kind mismatch (HKT) | ~6-8 | `cannot apply a type of kind ... as a type` under interpret |
| Existential escape | ~4 | `open: existential type variable escapes its scope` |
| Continuation already resumed | ~4 | one-shot continuation accounting differs from compiled |
| Preload self-conflict | ~2 | `defn 'X' already defined by an auto-loaded stdlib module` (e.g. `math.tur`) -- a W1 ordering bug |
| Misc tail | rest | unbound symbol, variadic rest typing, panic allow-list warnings, json natives |

> Counts are approximate (deduped on the first stderr line, ASan warnings
> filtered). Reproduce with the probe script in the reconciliation report; treat
> them as "shape of the work," not contract.

---

## Goal / definition of done

`tests/run-turi.sh` runs **every** fixture under `tur --interpret` (no
allowlist) and is **green**, with:

- `requires.compiled` / `requires.tur-only` the only meaningful skips,
- zero silent miscompiles hidden behind a carve-out without a tracking report,
- `tools/check_turi_parity.py` still passing (no EX_* regressions),
- the full compiled suite (`tests/run.sh`) still green.

### Non-goals

- Performance parity (the interpreter may stay 10-100x slower).
- Running user inline-C under the interpreter (permanent TI7 carve-out).
- Fixing the *compiled* path for any divergence found (file those separately;
  this plan only closes the interpreter side or carves it).

---

## Strategy: the fix-vs-carve decision rule

For each failing fixture, in order:

1. **Inline-C in the fixture body?** -> carve `requires.tur-only` with reason
   `inline-c` (W2). No investigation needed; the interpreter never runs user
   inline-C.
2. **Pure-turi, fails because a stdlib symbol is unbound?** -> belongs to W1
   (the prelude). Do **not** carve; it will be fixed wholesale.
3. **Pure-turi, runs to completion with WRONG stdout (rc=0) or wrong exit?**
   -> this is a **silent miscompile** (W4). Never carve silently: either fix it,
   or carve with `requires.tur-only` reason `interp-miscompile` **and** a
   one-paragraph `docs/reported/` entry. (CLAUDE.md: a test that surfaces real
   broken behavior is a finding, not a malformed test.)
4. **Pure-turi, hard error from a semantic divergence** (move/linearity, kind,
   existential, continuation)? -> W3: fix in `src/turi/eval.c` /
   `src/compiler/*` if tractable; otherwise carve with the specific reason and a
   report.

Carve reasons are recorded in the marker file body (one line), e.g.
`requires.tur-only` containing `inline-c` or `interp-miscompile: see
docs/reported/<slug>.md`. A short reason string keeps later audits cheap.

---

## Workstreams

### W1 -- Typed-stdlib prelude under `--interpret` -- **LANDED (conflict-free subset)**

**Shipped 2026-06-11.** `cmd_eval` now preloads a conflict-free typed-stdlib
subset via the `(load ...)` mechanism: `safe`, the `typeclass-*` stubs, and
`vec`/`slice`/`option`/`pair`/`tuple`/`list`/`grid`/`zipper`. Result:
`Cons`/`Option`/`Pair`/`Tuple` struct types and the `Eq`/`Clone`/`Functor`
classes resolve under `--interpret`. **+35 net fixtures** recovered (probe
660->695 pass), **zero regressions**, harness green at **181 passed**; full
compiled suite unchanged at 1573.

**Excluded on purpose -- the native-shim conflict.** `result`, `map`, `set`,
`hamt`, `mutmap`, and `contract` are **not** preloaded. The interpreter's
`native_*` shims (`native_ok`/`ok-val`/`result-map`, `native_set_count`, the
hamt invalidation guard, `tur-contract-check`) implement a **different
in-memory layout** than the real modules. Loading the module makes the
elaborator bind to the module defn while the runtime native reads the other
layout -- which regressed `coerce-carrier-to-struct` /
`hamt-transient-invalidated` / the `contract-*` fixtures and **heap-overflowed
`native_set_count`** (`src/main.c:5884`, a latent native bug W1 exposed).
`wk_eval_fixture` already skips `contract.tur` for the same reason. The
`ok?`/`err?`/`none?` elaborator stubs are kept (result.tur not loaded); `some?`
comes from the loaded `option.tur`; `vec-get`/`vec-set!`/`vec-free` stubs are
dropped (vec.tur owns them).

### W1b -- Reconcile the native-shim layer -- **Result LANDED (result.tur in the prelude)**

> The naive plan was "fix the layouts, then add result/map/set/hamt to the
> prelude." It is really a per-type representation-unification problem. For
> **Result** that unification is now done and `result.tur` is preloaded; the
> same three-piece pattern applies to map/set/hamt next.

**Result -- done.** Three pieces unified the int64-carrier-box and the
make-struct `TuriStruct` representations so `result.tur`'s typed, field-access
operations run under `--interpret`:

1. **Dual-rep readers** (`turi_struct_field` in eval.c/eval.h; `result_field`/
   `result_field_int` + `native_ok_pred`/`err_pred`/`ok_val`/`err_val` in
   main.c) accept both a make-struct `TuriStruct` (fields
   `[is_ok, ok_val, err_val]`) and the native `int64[3]` box.
2. **`native_result_eq`** invokes `result-eq?`'s two `^fat` comparison closures
   via `turi_call` (the inline-C `try_exec_simple_inline_c` could not run).
3. **`EX_GET_FIELD` carrier-box path** (eval.c): a struct that flowed via the
   int64 carrier ABI (e.g. a Result that was `:int` and then
   `(:: carrier (Result A B))`) reaches a field access as a `TURI_INT` pointer
   to an `int64[n]` box; field access now reads word `idx` from the box, tagged
   by the field's static type, instead of erroring "field access on non-struct".
   This was the last gate -- it is what makes `result.tur`'s `ok-val`/`err-val`
   accessors work on the box, not just on `make-struct` TuriStructs.

`result.tur` is now in the `cmd_eval` prelude (the `ok?`/`err?` stubs dropped,
since it defines them). **Recovered 5 fixtures** -- `typed/result-basic`,
`result-typed-basic`, `result-of-typed-eq` (also a W4 pure-turi silent
miscompile, fixed by this), `typed-slots/cs4-stdlib-helpers`,
`typed-slots/stdlib-container-layout` -- added to the allowlist (harness 919 ->
**924**, 0 failed; compiled 1573/0; zero regressions, incl. the allowlisted
`coerce-carrier-to-struct` which now passes via the carrier path).

**map/set/hamt -- hamt + set DONE; map blocked.** hamt.tur (raw `tur_hamt_*`
wrappers now registered in `cmd_eval`) and set (hamt-backed `set-*` natives over
`{void* hamt}`, fixing the `native_set_count` overflow; `#set{}` lowers through
the set ops) are landed and on the allowlist. **map** is blocked: its ops are
polymorphic `[K V]` defns, and a monomorphized polymorphic defn bypasses its
global native override, so the interpreter runs the module's inline-C body (a
`tur_hamt_*_eq_o` C call with a C-callback comparator) instead of the registered
`native_map_*`. Needs either native-override-of-monomorphized-poly-defns or a
turi-closure-aware HAMT. Full analysis:
[turi-map-set-hamt-interpreter-gap.md](../../reported/turi-map-set-hamt-interpreter-gap.md).
The original spike notes (still accurate for map) follow.

**Original spike (2026-06-11): the Result pattern does NOT directly transfer.**
Loading `hamt.tur`/`map.tur`/`set.tur` into the prelude does **not** regress the
allowlist (924/0), and the EX_GET_FIELD carrier path + `native_tur_hamt_*`
(which wrap the real runtime HAMT) are in place -- but the target fixtures still
do not work, for three reasons that make this materially harder than Result:

1. **~18 missing native ops.** `typed/map-basic` (`map-new`/`map-assoc`/
   `map-get`/`map-has?`/`map-count`/`map-free`/`map-dissoc`/`map-merge`) and
   `typed/set-basic` (`set-new`/`set-add`/`set-count`/`set-member?`/`set-free`/
   `set-remove`/`set-union`/`set-intersect`/`set-diff`/`set-eq?`) are inline-C
   wrappers around `tur_hamt_*` with no native overrides, so they hit
   "inline-C not supported" (map) or, worse, **heap-overflow** (`set-count` ->
   `native_set_count`, which still reads the `#set{}` `int64[2]` layout off a
   `{void* hamt}` set -- the documented bug).
2. **C-callback eq/hash mismatch (the real blocker).** The content-keyed HAMT
   ops (`tur_hamt_set_eq_o`/...) take the key-equality and hash as **C function
   pointers**. A native `map-assoc` receives the interpreter's `keyeq` as a
   *turi closure*, which cannot be handed to the runtime HAMT as a C callback.
   For `int` keys it would "work by luck" until a hash collision invokes the
   bogus callback (a latent crash -- "works by luck" is a bug, CLAUDE.md). A
   correct fix needs either a turi-closure-aware HAMT path or natives that
   re-implement the collision/eq logic in C over interpreter values.
3. **Literal-vs-module representation, no runtime tag.** `#set{}`/`#map{}`
   lower to one layout (`EX_SET_LIT` -> `int64[2]`) and `set.tur`/`map.tur` to
   another (`{void* hamt}`), routed through the same `set-count`/`map-count`
   natives with no way to tell them apart. Unifying requires changing the
   literal lowering too.

So map/set/hamt is its own focused sub-project, not a "land it now" change:
unify the value representation (route `#set{}`/`#map{}` through the HAMT path),
write the ~18 natives over `tur_hamt_*`, and resolve the C-callback eq/hash
(turi-closure-aware HAMT). hamt.tur itself (over the existing `native_tur_hamt_*`)
is the most tractable starting point. Full umbrella report (three gaps, repro,
fix directions):
[turi-map-set-hamt-interpreter-gap.md](../../reported/turi-map-set-hamt-interpreter-gap.md),
plus the narrower
[turi-native-set-count-layout-overflow.md](../../reported/turi-native-set-count-layout-overflow.md).

**Finding 1 -- adding the modules recovers nothing.** Adding `result.tur` to the
prelude (with the `ok?`/`err?` stubs dropped) **regressed** `coerce-carrier-to-
struct` and recovered **zero** fixtures: `typed/result-basic` still fails with
`inline-C not supported`, because its operations (`ok`/`err`/`result-map`/...)
are inline-C bodies the interpreter cannot execute, and the `native_*` overrides
do not cover all of them (and where they do, they use a different layout). So
"load the module for its struct type" does not help; the *operations* must be
interpretable, which means **complete, layout-consistent native coverage per
type**, not module loading.

**Finding 2 -- four incompatible representations of the "same" value.** A
Result/Set/Map value can exist in the interpreter as any of:

1. a `native_*` int64-box (e.g. `native_ok` -> `int64[3] {is_ok, ok, err}`),
2. a `make-struct`/`defstruct` `TuriStruct` (heap object with metadata),
3. a `#set{}` / literal layout (`EX_SET_LIT` -> `int64[2] {ptr, count}`),
4. the real module's struct (e.g. `set.tur`'s `{void* hamt}`).

The `native_*` shims silently assume one shape; feeding them another silently
miscompiles or heap-overflows (`native_set_count`, see
[docs/reported/turi-native-set-count-layout-overflow.md](../../reported/turi-native-set-count-layout-overflow.md)).
Crucially the **same `set-count` native serves both `#set{}` literals (correct)
and `set.tur` sets (overflow)** -- with no runtime tag to tell them apart -- so
the shims cannot simply be repointed.

**What W1b actually requires (per type: Result, Option, Map, Set, Hamt).** Pick
**one** in-memory representation and make every producer and consumer agree:
`make-struct`/`#lit`/native-constructor all build it, and every accessor
(native or module) reads it. Concretely, for each type either:

- (a) give the interpreter a **complete native override set** (constructor +
  every operation) over one chosen layout, and make `make-struct <T>` /
  `EX_*_LIT` produce that same layout; **or**
- (b) make the module operations **interpretable without inline-C** (so loading
  the module is sufficient and no native is needed) -- only viable for modules
  whose ops are pure-turi or reducible to existing natives.

This is a multi-session, per-type effort and should be sequenced **after** W2
(carve) and W3 (semantic fixes), which are independently tractable and move the
flip closer. Suggested first target: **Result** (highest fixture count, and its
struct layout `{bool is_ok; int64 ok; int64 err}` already matches the native
int64-box -- the gap is only that `make-struct Result` builds a `TuriStruct`
instead of that box).

**Adjacent bug surfaced during the spike (W4, independent):**
`ic_exec_accessor` silently miscompiles `return p == NULL || !p->field;` (drops
the `!`/`||`), inverting predicates like `result-basic`'s `u-err?` -- filed at
[docs/reported/turi-inline-c-accessor-miscompiles-boolean-returns.md](../../reported/turi-inline-c-accessor-miscompiles-boolean-returns.md).

---

#### Original W1 design (for reference)

**Problem.** `cmd_eval` preloads only `macros.tur` + benchmark stubs; the
compiled path (`compile_to_c`, `src/main.c:646`) auto-loads the full
typeclass-stub + typed-collection set. So `Cons`/`Option`/`Result`,
`Eq`/`Clone`/`Hash`, and `map-new`/`vec-eq?`/`tcons`/... are unbound under the
interpreter. Recovers the `typed/*`, `clone-*`, `make-struct`, several
`typeclass not defined`, and `unknown function` buckets (~93+ pure-turi, plus it
unblocks many of the ~155 empty-stderr that fail *after* a missing symbol).

**Approach (de-risked -- three issues found in the 2026-06-11 spike):**

1. **Load via `(load "...")`, not `turi_eval_file`.** Several prelude modules
   (`safe.tur`, `contract.tur`) carry their own `defmodule`; only the `(load)`
   preprocessing assigns distinct `file_id`s so the per-file `has_defmodule`
   reset fires. (Same root cause as the landed macros fix, scaled up.) Build one
   `(load A)(load B)...` source in dependency order and `turi_eval` it once.
2. **Drop the benchmark stubs the real modules provide.** `cmd_eval` injects
   no-op `vec-get`/`vec-set!`/`hamt-*`/`ok?`/`some?`/... stubs for benchmark
   scripts that do not load stdlib; the real modules then fail to elaborate with
   `defn 'vec-get' already defined by an auto-loaded stdlib module`. Keep only
   the genuinely benchmark-only stubs (`run-ring`/`run-nbody`/`io-*`/
   `random-access-bench`/`cstr->parse-int`/`bit-*`/...) and re-check the
   benchmark fixtures (most self-provide local inline-C stubs per CLAUDE.md).
3. **Cost.** ~24 modules add ~300ms per interpreter invocation (the interpreter
   re-elaborates all accumulated source each `turi_eval`). Acceptable for
   one-shot script runs; for the REPL, consider a cached/precompiled prelude
   (out of scope for the flip, file as a follow-up).

**Reference.** `wk_eval_fixture` (`src/main.c:6671`) already preloads the typed
set via `turi_eval_file` -- reuse its dependency ordering, but switch it to
`(load)` and unify the two prelude sites so they cannot drift.

**Validation.** `typed/list-basic`, `typed/option-basic`, `typed/map-basic`,
`clone-list` pass under `--interpret`; the 145-fixture allowlist stays green;
re-run the probe and confirm the recovered count.

### W2 -- Carve the inline-C set -- **LANDED (via auto-detection, not markers)**

**Problem.** User inline-C is a permanent TI7 carve-out the interpreter does not
run. Post-W1/W3 re-measure (non-`errors/`, under `--interpret`): **325 inline-C
clean-fail**, **25 inline-C silent-miscompile**, 244 pure-turi fail, 678 pass.

**Decision -- auto-detect, do not drop ~350 marker files.** The plan's original
"drop a `requires.tur-only` marker per fixture" would mean ~350 noisy files needing
maintenance. Instead `run-turi.sh` gained `fixture_has_inline_c`: a non-allowlisted
fixture whose body has a ` ```c ` block is classified an **inline-c carve-out**
(`SKIP_INLINEC`) rather than an allowlist gap. This is self-maintaining (new
inline-C fixtures auto-carve) and is the exact mechanism the W5 flip will use to
skip the inline-C set. The ~15 inline-C fixtures that *do* work under turi
(`inline-c-binop`, `gen-*`, ...) are on the allowlist and are checked first, so
they still run. `run.sh` ignores `requires.tur-only`/`SKIP_INLINEC` entirely, so
compiled coverage is untouched.

**Shipped.** The harness summary now reports the carve-outs separately:
`463 passed, 0 failed, 1206 skipped -- of which 427 inline-c carve-outs ... 779
non-inline-C not yet on the allowlist`. That **427** is the W2 carve; the **779**
is the honest W5 triage surface (many already pass and just need adding; ~244 are
pure-turi failures needing a fix or marker).

**The 25 silent-miscompile inline-C fixtures are NOT hidden silently.** They are
filed in
[docs/reported/turi-inline-c-silent-miscompiles.md](../../reported/turi-inline-c-silent-miscompiles.md):
`try_exec_simple_inline_c` claims their body and returns wrong output (rc=0).
They carve as inline-C for the flip, but the evaluator bug is W4 work (make the
`ic_exec_*` matchers refuse shapes they cannot evaluate, then they error cleanly).

**Validation.** `check_turi_parity.py` unaffected; harness green at 463/0; full
compiled suite unchanged at 1573/0.

### W3 -- `errors/` diag coverage + semantic divergences -- **MOSTLY DONE**

**The "move/linearity divergence" bucket did not exist as bugs.** Investigation
(2026-06-11) found that all ~33 move/linearity "failures" were a **probe
artifact**: they are `tests/fixtures/errors/*` *negative* fixtures that assert a
diagnostic via `expected.diag` (not stdout/exit), so the probe -- which compared
stdout/exit -- mis-scored them, and `run-turi.sh` skipped the `errors/` tree
wholesale (the TI0-noted gap). The interpreter shares the elaborator, so the
affine/linear/move checks run identically: `errors/linear-dropped` emits the
exact `TUR-E0100 linear value 'x' dropped...` under `-Xlinear --interpret`.

**Shipped:** `run-turi.sh` now runs every `errors/*` fixture under
`--interpret` and does run.sh's substring diag comparison (new
`run_turi_error_fixture` + `TURI_ERRORS_DENY`). **282 of 298 pass** (the whole
move/linearity/affine/type-error surface is now CI-validated under turi);
harness went 181 -> **463 passed, 0 failed**. The `.gitignore` was extended to
cover nested `tests/fixtures/*/*/turi.{stdout,stderr}` scratch.

**Remaining: 9 genuine divergences** (denylisted, tracked in
[docs/reported/turi-error-fixture-diag-divergences.md](../../reported/turi-error-fixture-diag-divergences.md)):
3 reporting-stage (unbound-call / heterogeneous-map error at runtime, empty
stderr, no elab diag), 4 missing-check (`lifetime-cyclic` TUR-E0106, reader-macro
strict-collision, `#lang` unknown/not-implemented run clean), 2 TI3.2 carve-outs
(`serial-context-*` TUR-E0706). Fix or leave carved per that report.

**Other small semantic buckets** (from the non-errors probe; verify they did not
shrink after W1 before acting):

- **Kind mismatch (~6-8)** and **existential escape (~4).** HKT / existential
  elaboration state not threaded into the interpreter the same way. Pair with
  whoever owns `elab` HKT.
- **Continuation already resumed (~4).** One-shot/affine continuation accounting
  in `eval.c` differs from the compiled `cps_prompt` semantics.
- **`if condition must be bool` (~5 pure-turi).** A predicate native's return
  type defaults to `:int` when its module is not loaded; mostly subsumed by W1,
  re-measure after W1.

### W4 -- Silent miscompiles (highest severity) -- **IN PROGRESS (accessor class fixed)**

The silent-miscompile surface was measured precisely (post-W1/W3, `rc` matches
expected but output is wrong):

- **25 inline-C** silent miscompiles via `try_exec_simple_inline_c` (carve-outs
  for the flip, but a real evaluator-trust bug).
- **12 pure-turi** silent miscompiles (NOT carve-able; these block W5): 11 real
  interpreter bugs + 1 legit reader-conditional carve (`reader-cond`).

**Landed:** the `ic_exec_accessor` boolean-return class. The accessor now
**refuses** any field-access return containing a result-transforming operator
(`||`/`&&`/`==`/`!=`/unary-`!`/`<`/`>`, `->` skipped) instead of silently reading
the bare field -- turning silent-wrong into a clean "inline-C not supported"
error for *any* program with that shape. Fixed 3 of the 25 inline-C cases
(incl. `result-basic`) with **zero regressions** (allowlisted `inline-c-binop` /
`gen-*` still pass; harness 463/0; compiled 1573/0). Reports:
[turi-inline-c-accessor-miscompiles-boolean-returns.md](../../reported/turi-inline-c-accessor-miscompiles-boolean-returns.md)
(FIXED) and
[turi-inline-c-silent-miscompiles.md](../../reported/turi-inline-c-silent-miscompiles.md)
(3 fixed, 22 remain via other `ic_exec_*` matchers).

**Remaining W4:**

- **22 inline-C** evaluator miscompiles via the other matchers (constructor /
  snprintf / switch-string / linked-list / simple-return) -- e.g. the
  `backtrack-*` (7), `show-*` (3), `arrow-instance-*` (2) clusters. Apply the
  same refuse-rather-than-guess tightening per matcher. Inline-C carve-outs, so
  they do not block W5 -- but they ship a wrong-answer hazard for real programs.
- **11 pure-turi interpreter bugs** (the W5 blockers), catalogued in
  [turi-pure-turi-silent-miscompiles.md](../../reported/turi-pure-turi-silent-miscompiles.md).
  **7 fixed** (added to the allowlist, harness 912 -> 919): `EX_ASCRIBE`
  primitive coercion (`rt-return-dispatch-*`), `EX_ANY_TYPE_OF` coarse tags +
  `EX_ANY_CAST` checked downcast (`any-box-*`, `any-cast-mismatch-panic`),
  `catch-unwind` firing unwound defers (`panic-catch-unwind-defer`), and
  `native_extern_puts` (`extern-c-spaced-typeann`). `reader-cond` carved
  `requires.compiled` (legit `#?(:tur/:turi ...)` path-divergence). **4 remain**,
  each deep and overlapping another workstream: `result-of-typed-eq` (W1b --
  Vec/Result recursive eq), `range-bound-show-ord` (inline-C conditional-snprintf
  matcher gap), `codegen-private-defn-collision` (module-private name mangling --
  core module-system change), `rc-unique-violation` (rc strong/weak-count check
  in `ref/from-rc`).

### W5 -- The flip itself

**Groundwork done.** The harness already (a) auto-skips inline-C carve-outs (W2),
(b) skips all `requires.*` markers before the allowlist check, (c) runs `errors/*`
with diag comparison (W3), and (d) has every passing non-inline-C fixture on the
allowlist (the bulk-add). So the allowlist now == "everything that works," and
the flip is mechanically: replace "in allowlist?" with "not failing." The only
thing standing between here and a green denylist is the **260** remaining
allowlist-gap fixtures (~244 genuine failures) -- each must be fixed (W1b/W4) or
carved with a marker.

Once W1-W4 leave only carved fixtures failing:

1. Delete `TURI_FIXTURES_DEFAULT` from `tests/run-turi.sh`; default to
   "run every fixture minus `requires.{compiled,tur-only,dedicated-runner,
   spices}`" (inline-C auto-carve and the marker skips already in place).
2. Retire the `KB-001` allowlist-gap workaround comment.
3. Flip `tests/run-flags.sh`'s three `tur run` assertions (`:345` try-with-basic,
   `:355` try-with-nested, `:408` effect-export-explicit) to `--interpret`.
4. Confirm `run-turi.sh` is green with near-zero non-marker skips, and the full
   compiled suite is unchanged.

---

## Sequencing

```
W1 (prelude, DONE)
      ├──► W3 (semantic fixes) ──┐
      ├──► W4 (silent miscompiles)┤──► W2 (carve inline-C) ──► W5 (flip)
      └──► W1b (per-type repr.) ──┘         ▲
                 (large, multi-session;     │
                  re-measure after each before carving)
```

W1 landed (the conflict-free subset). **W1b was rescoped** (see its section): it
is *not* a module-load step but a per-type representation-unification effort, so
it moves **after** the independently-tractable W3 (semantic fixes) and W4
(silent miscompiles) and is sequenced alongside W2 rather than gating it. W2 is
the mechanical bulk that makes the flip reachable. **Re-measure after each step**
before carving anything. W5 is last and is itself the acceptance test.

Suggested PR slicing (each independently green):
1. W1 prelude (+ allowlist additions for newly-passing typed fixtures). **DONE.**
2. W3 move/linearity root-cause fix (likely one fix, ~33 fixtures). **Recommended
   next -- most bounded high-leverage piece.**
3. W4 batch 1: `weak-dangling` root cause; `ic_exec_accessor` boolean-return
   miscompile (`result-basic`).
4. W2 carve sweep + remaining W4 carves-with-reports.
5. W1b per-type representation unification (start with Result), as capacity
   allows -- recovers the typed-collection cluster but is the heaviest piece.
6. W5 flip + run-flags wiring.

---

## Risks

1. **Prelude perf / REPL latency (W1).** ~300ms/invocation. Mitigate by
   unifying the two prelude sites and, if needed, a follow-up cached prelude.
   Do not let perf block the flip -- the harness has a stamp cache.
2. **Carving hides a real bug (W2/W4).** The decision rule forbids carving a
   silent miscompile without a report. Enforce by scripting the carve to *skip*
   any rc=0-wrong-output fixture and list them for W4.
3. **Prelude changes ripple to currently-green fixtures (W1).** Some passing
   fixtures may rely on the absence of a stdlib symbol or on a benchmark stub.
   Gate every W1 PR on the 145-fixture allowlist staying green + the full
   compiled suite.
4. **Move/linearity fix changes interpreter semantics broadly (W3).** If the
   interpreter stops enforcing linearity, ensure no fixture *relies* on the
   interpreter catching a linearity error (check the `errors/` subtree handling
   noted in TI0).
5. **Scope creep into the compiled path.** Several divergences are the
   interpreter being *stricter* or *wronger* than codegen; resist "fixing" the
   compiler here -- file separately.

---

## Validation / metrics

Track three numbers per PR (all from the probe + harness):

- **probe pass/fail/skip** -- the headline (660/910/92 today; target fail -> 0
  non-carved).
- **`run-turi.sh` summary** -- must stay green every step (145 passed today).
- **`check_turi_parity.py`** -- must stay `0 gaps`.
- **`tests/run.sh`** -- must stay `1573 passed, 0 failed` (count drifts with
  fixture churn).

Definition of done = W5 merged with `run-turi.sh` green at denylist default.

---

## See Also

- [turi-parity-post-v1-plan.md](turi-parity-post-v1-plan.md) -- parent plan
  (TI8.b lives there; this is its execution detail).
- [docs/reported/turi-harness-flip-reconciliation.md](../../reported/turi-harness-flip-reconciliation.md)
  -- the 31 allowlist reconciliation + the probe + the defmodule fix writeup.
- [docs/reported/turi-harness-compiles-instead-of-interpreting.md](../../reported/turi-harness-compiles-instead-of-interpreting.md)
  -- the resolved root blocker.
- [docs/turi-carve-out.txt](../../turi-carve-out.txt) -- EX_* carve-out list the
  ratchet enforces.
- `src/main.c` `cmd_eval` (`:4801`), `compile_to_c` prelude (`:646`),
  `wk_eval_fixture` (`:6671`) -- the three prelude sites to unify in W1.
