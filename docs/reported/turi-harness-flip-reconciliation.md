# TI8 harness flip: allowlist reconciliation + full-denylist blast radius

> **Next big lift (2026-06-13):** the largest remaining `--interpret` gap is
> json + schema (19 `schema-*` + 5 `json-reader-*` fixtures) -- a self-contained
> JSON parser/AST + schema decoder engine (~70 inline-C functions), materially
> bigger than the sym/seq work and its own dedicated PR. Inventory + layered
> approach (do the json layer first; it also unblocks `json-reader-*`) is captured
> in
> [docs/upcoming/turi-json-schema-interpreter-plan.md](../upcoming/turi-json-schema-interpreter-plan.md).

> **Progress (benchmark-stub collision + math helpers, 2026-06-13):** the
> `cmd_eval` benchmark-stub block injected `(defn int->float ...)` /
> `(defn cstr->parse-int ...)` placeholders, so a fixture that
> `(load ...)`ed `math.tur` / `str.tur` tripped the "already defined by an
> auto-loaded stdlib module" guard. Both names are native-backed (the native
> resolves the bare call at elaboration -- verified), so the stubs were
> **redundant and removed**, clearing the collision. Added `float->int` / `sqrt`
> / `floor` natives so the loaded math helpers run; `stdlib-float-convert-load`
> and `load-in-imported-module` now pass and join the allowlist (harness 1090 ->
> 1091). `errors/unknown-helper-load-hint` moved to `requires.compiled`: the
> "unknown helper -> load math.tur" hint is a compile-path diagnostic, and under
> `--interpret` the math helpers are pre-registered natives (for stdlib-free
> benchmark scripts) so `float->int` is no longer "unknown" there. The other two
> ex-collision fixtures stay off the allowlist as genuine inline-C carve-outs:
> `reader-macros-rx-literal` drives `re.tur`'s regex engine and
> `sum-either-str-parse` uses `str->int-checked`'s `strtoll` + `ctor_Left/Right`
> inline-C. No codegen change.
>
> **Progress (lazy Seq + generator early-return fix, 2026-06-13):** **the whole
> `seq/*` lazy-sequence library now runs under `--interpret`** -- 41 `seq-*`
> fixtures plus `gen-collect` join the allowlist (harness 1048 -> 1090). seq's
> inline-C bridges assume the *compiled* ABI (fat-closure calls via a C function
> pointer, a `{__state,__next_fn}` generator struct, malloc/free growable
> arrays), so the tree-walker declined them. `wk_register_seq_natives`
> (`src/main.c`) re-implements the bridge surface over the interpreter's value
> model: `seq-iter`/`seq-gen-next`/`seq-gen-done` drive a `TURI_GEN` via a new
> public `turi_gen_advance_val` (the yield protocol already hands back a pointer
> to the value box, so no re-boxing); the `seq-call-*` bridges invoke the
> `TURI_CLOSURE` callback via `turi_call`; and the option/out-vec/cons/Tuple2 +
> `gen-arr` `{len,cap,data}` helpers are nativized as a consistent set
> (producers and accessors together). A real interpreter bug surfaced and was
> fixed: a `(return)` inside a generator body (e.g. `seq/take-while`'s early
> stop) left `env->returning` set, and since the gen coroutine shares the
> consumer's `TuriEnv`, the driver loop saw it and bailed -- returning
> `env->return_value` (0) instead of its accumulator (`seq-pipeline-foldl`
> printed 0 for 161700). `gen_body_thunk` now resets `returning` after the body
> completes, mirroring the existing `throwing` reset. `seq-builders-unfold` /
> `seq-core-from-vec` stay carved (their fixtures define their own inline-C).
> No codegen change.
>
> **Progress (EX_SYM_LIT + native sym ops, 2026-06-13):** **first-class `:Sym`
> values (`-Xsymbols`) now work under `--interpret`** -- all 4 sym fixtures
> (`sym-stdlib`, `quoted-keyword-type-ann`, `sym-map-key`, `sym-dynamic`) pass
> and are on the allowlist (harness 1044 -> 1048). `EX_SYM_LIT` was a documented
> carve-out (kind 114, "unhandled expression kind"); it now has a case arm in
> `src/turi/eval.c` that re-interns the literal's name into `env->st` and carries
> the stable `const Symbol *` as the int64 carrier -- interning by name gives
> pointer-identity `Eq[Sym]`/`Hash[Sym]` and makes the `str->sym` round-trip
> agree with literals. `wk_register_sym_natives` (`src/main.c`) overrides the
> inline-C `sym.tur` bodies the tree-walker cannot run: `sym->str` (reads
> `Symbol->name`), `sym=?` / `__inst_Eq_eq_qu_Sym` (pointer identity), `str->sym`
> (interns into the same `env->st`, so it wins over `sym-dynamic.tur`'s
> runtime-table inline-C and stays pointer-consistent with literals),
> `__inst_Hash_hash_Sym`, and the `MapKey[Sym]` `mk-cmp`/`mk-box` carriers
> (symbols key by pointer identity, reusing the int-carrier comparator).
> `EX_SYM_LIT` was removed from `docs/turi-carve-out.txt` (now handled; parity
> 113/115, 2 carved); `EX_CONS_LIST` stays carved. No codegen change.
>
> **Progress (inline-C ADT-carrier re-tag, 2026-06-13):** **fixed a silent
> interpreter bug that broke the entire `range-*` family** (14 fixtures all
> failing `match: no arm matched`) and added the 12 fixable ones to the
> allowlist (harness 1032 -> 1044). **Root cause:** user ADT/GADT values are
> `TURI_STRUCT` in the tree-walker (`adt_ctor_native` -> `make_struct_val`), but
> an inline-C function declared to *return* such a type round-trips the
> `TuriStruct*` through an `int64_t`. `range.tur` packs two `Bound` endpoints
> into a heap struct via inline-C (`range-new`), then reads them back with
> `range-lower`/`range-upper` (`: Bound`); the simple inline-C executor handed
> the field back as a bare `TURI_INT`, so the struct tag was lost and every
> downstream `match` over `Bound` (which checks `tag == TURI_STRUCT`) found no
> arm. **Fix** (`src/turi/eval.c`, the unified inline-C return point): when the
> function's declared `return_type.kind` is `TY_ADT`/`TY_STRUCT` and the executor
> produced a non-null `TURI_INT`, reinterpret the carrier as the original
> `TuriStruct*` (`turi_struct_val`). The int64 *is* the original pointer (it came
> in as `args[i].as_int` via the union, was stored verbatim, and returned), and
> every interpreter ADT is struct-backed, so the reinterpretation is sound; the
> non-null guard avoids a NULL-deref on a genuine nil carrier. `range-show` and
> `range-from-range*` stay carved (genuine inline-C: snprintf %s formatting / a
> `seq/from-range` body the simple executor declines). No codegen change.
>
> **Progress (W5 bulk-add, 2026-06-12):** **30 verified-passing non-inline-C
> fixtures joined the `run-turi.sh` allowlist** (harness 1002 -> 1032 passed, 0
> failed). A sweep of the `SKIP_ALLOWLIST` coverage gap (the genuine W5 triage
> surface) ran every positive (non-`errors/`) gap fixture under `--interpret`
> *with its `flags`* and pinned the ones whose stdout + exit already match the
> compiled expectation under true interpretation -- no works-by-luck carrier
> accident, all produce non-trivial output. The added families: `cloneable-*`
> (8), `hkt-row-*` (5), `data-literal-*` / `vec-eq-ascribed*` (5), `sized-*`
> accept-side (3), `macro-*` (3), plus `refined-nonempty`,
> `typeclass-poly-wrapper-struct-receiver`, `top-level-def-init-runs-before-main`,
> `defn-class-constraint-list-syntax`, `cross-module-macro-vec-arg-in-wrapper-body`,
> `workflow-roundtrip`. This shrinks the allowlist->denylist gap by 30 with zero
> source/codegen change (allowlist-only); the residual positive gap is fixtures
> that genuinely fail under `--interpret` (need a fix or a `requires.tur-only`
> carve before the flip).
>
> **Prereq decomposition (2026-06-12):** the "de-risked roadmap" below is broken
> into independently-landable groundwork (native-registry parity diff,
> benchmark-stub overlap audit, opt-in `TUR_TURI_FULL_PRELUDE` flag, carve
> markers for the move/linearity + `if-bool` divergences) in
> [docs/upcoming/v1/turi-open-reports-prereqs.md](../upcoming/v1/turi-open-reports-prereqs.md).

**Summary:** `tests/run-turi.sh` was flipped from `tur run` (which compiles and
runs a native binary) to `tur --interpret` (the actual tree-walking
interpreter), resolving the blocker in
[turi-harness-compiles-instead-of-interpreting.md](../archive/history/turi-harness-compiles-instead-of-interpreting.md).
True interpretation turned **31 of the 146 allowlisted fixtures red**; those 31
were removed from the allowlist (they were never real interpreter coverage).
This report catalogues the 31 by root cause and records the measured blast
radius of the *full* allowlist->denylist flip (still future work): under
`--interpret`, **933 of ~1500 fixtures fail**.

**Severity:** Mixed. Some removed entries are permanent carve-outs (call/cc,
inline-C); several are real interpreter gaps (missing stdlib natives / struct
types) and a handful are **silent miscompiles** (the interpreter returns wrong
values, rc=0) -- the worst class, previously hidden by the compile-based
harness.

## What changed

- `tests/run-turi.sh` now runs each fixture with `"$TUR" $flags --interpret
  "$input"` instead of `... run "$input"`.
- The 31 false-green entries were removed from `TURI_FIXTURES_DEFAULT`. The
  harness is green again at **115 fixtures + 7 async eval scripts = 122 passed,
  0 failed**.
- `tools/check_turi_parity.py` + `docs/turi-carve-out.txt` were added and wired
  into `tests/run.sh` as a pre-test CI ratchet (the EX_* parity gate).

## The 31 removed entries, bucketed

### A. Permanent carve-outs (do not re-add without the underlying feature)

- **call/cc (EX_CALLCC, CPS carve-out):** `call-cc-star`,
  `continuation-callcc`, `continuation-escape`, `continuation-escape-fn`.
  Tracked with `EX_CALLCC` in `docs/turi-carve-out.txt`.
- **User inline-C (TI7 carve-out):** `ptc4-basic`
  (`inline-C not supported in interpreter mode`), `effect-capture-k`
  (capturing-continuation path; aborts under the interpreter).

### B. Real interpreter gaps -- missing stdlib natives / struct types

The interpreter does not register the typed-stdlib natives and struct types
that the compiled prelude provides, so any fixture that touches `stdlib/typed/*`
or the `Clone` typeclass errors out:

- **`make-struct: '<T>' is not a defined struct type`:** `typed/list-basic`
  (`Cons`), `typed/option-basic` (`Option`), `typed/result-basic` (`Result`).
- **`unknown function` / `unbound variable`:** `typed/map-basic`,
  `typed/map-collision`, `typed/map-eq` (`map-new`), `typed/set-basic`
  (`set-new`), `typed/vec-basic` (`vec-eq?`), `typed/slice-basic`
  (`slice-eq?`), `typed/grid-basic` (`grid-new`), `typed/zipper-basic`
  (`zipper-new`), `typed/pair-basic` (`pair-fst`), `typed/list-macro`
  (`tnil?`).
- **`Clone` typeclass not defined under interpret:** `clone-primitives`
  (`no typeclass method found for 'clone'`), `clone-list`, `clone-option`,
  `clone-pair` (`typeclass 'Clone' is not defined`).
- **arrow/HKT stdlib instances:** `arrow-instance-stdlib-basic`,
  `hkt-stdlib-result-ok-biased` (rc=1 -- stdlib instance resolution under
  interpret).

These are tractable but each needs native registration / struct-type seeding in
`src/turi/`; they are the bulk of the full-flip work (see blast radius below).

### C. Silent miscompiles -- interpreter returns WRONG values (rc=0)

These ran to completion under `--interpret` but produced wrong output. The
compile-based harness hid them entirely:

| Fixture | Interpreter output | Expected |
| --- | --- | --- |
| `result-basic` | `... false/true swapped; 0 where 99 expected` | correct Result values |
| `weak-dangling` | `true` (weak still "live") | `false` (dangling) |
| `instance-head-hole-pair` | `0` / `0` | `42` / `7` |
| `arrow-instance-apply` | rc=0, stdout mismatch | match |

`weak-dangling` is the most concerning: the interpreter's weak-ref liveness
check disagrees with the compiled semantics. These deserve individual
root-cause reports when the full flip is tackled.

### D. Dynamic-variable conveyance

- `dynvar-convey`, `dynvar-convey-isolation`: rc=1 under interpret (dynamic-var
  conveyance across the async/fiber boundary is not wired in the interpreter).

## Full allowlist->denylist flip: measured blast radius

Running **every** `tests/fixtures/*` under `--interpret`, minus those carrying
`requires.{compiled,tur-only,dedicated-runner,spices,tsan}` (92 skipped), gives:

```
pass=637  fail=933  skip=92
```

The 933 failures cluster into (first stderr line, deduped):

- ~396 with no stderr (stdout mismatch or abort -- includes the silent
  miscompiles in bucket C, scaled up),
- 46 `only one defmodule is allowed per file` (interpreter re-processes
  defmodule; likely a real interpreter defect in module loading),
- 43 `typeclass '...' is not defined` originating in `stdlib/schema.tur`,
  `stdlib/range.tur`, `stdlib/str.tur` (typeclass registration during stdlib
  load under interpret),
- 15 `no typeclass method found`,
- 13 `inline-C not supported` (genuine TI7 carve-outs),
- 13 `if condition must be bool, got int` (interpreter type-check divergence),
- 12 `unknown function` + several `unbound variable: {vec-of,set-of,hamt-of,
  map-count,grid-new,...}` (missing-native gap -- the plan's TI8 item about
  diffing the compiler's builtin-native registry against the interpreter's),
- ~20 use-after-move / linear-dropped / linear-used-after-consume (the
  interpreter runs the move/linearity checker and diverges),
- 10 `make-struct: '...' is not a defined struct type`,
- plus a long tail (kind mismatch, existential escape, reader-macro, etc.).

## TI8.b progress: defmodule concatenation defect fixed

The `46x only one defmodule is allowed per file` bucket was root-caused and
**fixed**. Root cause: `cmd_eval` (the `--interpret` entry) preloaded
`macros.tur` via `turi_eval_file`, which **concatenates** the source into a
single `<eval>` blob with `file_id = 0`. `macros.tur` carries `(defmodule
tur/macros ...)`, so when a user fixture *also* declared a defmodule, both forms
landed in `file_id 0` and the per-file `has_defmodule` reset
(`elab_toplevel.c:1183`, which keys on a `span.file_id` change between
consecutive forms) never fired -- tripping the one-defmodule-per-file check in
`elab_module.c:553`.

Fix (`src/main.c`, `cmd_eval`): preload `macros.tur` via a `(load "...")` form
instead of `turi_eval_file`. The `(load ...)` preprocessing assigns the loaded
file its own `file_id`, so the boundary reset fires and a user defmodule no
longer collides with the preloaded one. Verified: `defmodule-fat-fn-param-export`
/ `defmodule-pap-forward-ref-fat-fn` and the `module-*` family now pass under
`--interpret`. Post-fix probe: **660 pass / 910 fail / 92 skip** (was 637/933).
23 module/defmodule fixtures were added to the `run-turi.sh` allowlist (TI8.b
block); the harness is green at **145 passed, 0 failed**.

## De-risked roadmap for the remaining recovery (next session)

The biggest *recoverable* bucket is the typed-stdlib cluster (typed/*, clone-*,
make-struct, map-new/...): the interpreter does **not** preload the
typed-collection + typeclass-stub modules that the compiled path auto-loads in
`compile_to_c()` (`src/main.c:646`), so `Cons`/`Option`/`Result`, the
`Clone`/`Eq`/`Hash` classes, and `map-new`/`vec-eq?`/`tcons`/... are unbound
under `--interpret`. An experiment preloading that set surfaced three issues
that must be handled together before it can land:

1. **Use the `(load ...)` mechanism, not `turi_eval_file`.** Several modules
   (`safe.tur`, `contract.tur`) carry their own defmodule; only `(load ...)`
   assigns distinct file_ids so the per-file reset keeps them from colliding
   (same root cause as the fix above, scaled up).
2. **Drop the benchmark stubs that real modules provide.** `cmd_eval` injects
   no-op stubs (`vec-get`, `vec-set!`, `hamt-*`, `ok?`, `some?`, ...) for
   benchmark scripts that do not load stdlib. The real modules then fail to
   elaborate with `defn: 'vec-get' is already defined by an auto-loaded stdlib
   module`. The overlapping stubs must be removed (keeping only the genuinely
   benchmark-only ones: `run-ring`/`run-nbody`/`io-*`/`random-access-bench`/
   `cstr->parse-int`/`bit-*`/...), and benchmark fixtures re-checked (most
   already self-provide local inline-C stubs per CLAUDE.md).
3. **Cost.** Preloading ~24 modules adds ~300ms per interpreter invocation and
   makes the full probe run ~6x slower (the interpreter re-elaborates all
   accumulated source each `turi_eval` call). Acceptable for one-shot script
   runs; consider a cached/precompiled prelude for the REPL.

`wk_eval_fixture` (`src/main.c:6671`) already preloads the full typed-stdlib set
via `turi_eval_file` -- it is a useful reference, but note it would hit the same
defmodule-concatenation issue and should be re-validated when the prelude path
is unified.

The other large buckets stay as-is for now: typeclass-registration gaps during
stdlib load (schema/range/str), the move/linearity-checker divergence (~20),
`if condition must be bool` type-check divergence, genuine inline-C carve-outs,
and the silent miscompiles in bucket C (which deserve individual root-cause
reports). Each must be fixed in `src/turi/eval.c` or carved with
`requires.tur-only` before the allowlist->denylist flip can land green.

## Remaining wiring (follow-up)

- `tests/run-flags.sh` still calls `tur run` for three specific assertions
  (`:345` try-with-basic, `:355` try-with-nested, `:408` effect-export-explicit).
  Left as-is here to keep scope contained; flip them to `--interpret` when their
  interpreter behavior is confirmed.

## Status

Filed while executing TI8 of
`docs/upcoming/turi-parity-post-v1-plan.md`. TI8.a (parity ratchet +
harness-now-interprets + honest allowlist) and the TI8.b defmodule fix have
landed; the full allowlist->denylist flip remains, with the de-risked roadmap
above and the measured per-bucket scope.
