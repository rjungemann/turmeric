# Plan: relocate the remaining stdlib inline-C natives into libturi

**Status:** proposal (not started). **Area:** `src/main.c`, `src/turi/`
(tree-walking interpreter + embedding library).
**Goal:** make the rest of the stdlib -- `option` / `result` / `list` / `str` /
math, the typeclass-instance overrides, `seq`, `json` / `schema`, the
concurrency + OS handle modules, and `sym` -- usable on **every** libturi
interpreter env (embedders using `turi_eval`, the WASM REPL, the interpreter
test harnesses), the same way the collection natives already are, so a bare
embedder that loads a stdlib module no longer hits "inline-C not supported in
interpreter mode" the moment an op bottoms out in a native override.

This is the direct follow-on to
[turi-interp-collections-libturi-plan.md](turi-interp-collections-libturi-plan.md),
which moved the `Vec` / `Set` / `Map` / `HAMT` natives into `tur_core`, and to
the `set.tur`/`map.tur` `(load ...)` dependency fix
([docs/archive/interp-load-set-map-elaboration-gap.md](../archive/interp-load-set-map-elaboration-gap.md)).
Those made `Set`/`Map` elaborate and round-trip through an embedder; but any
stdlib op whose inline-C body is backed by a *still-CLI-only* native (e.g.
`option-map`, `result-flat-map`, a `seq` transform, a `json` decode) remains
broken for `libturi` consumers. This plan closes that gap for the rest of the
surface, using the exact pattern collections proved out.

## Background -- the actual current state

The collection relocation is done. What is left: **~283 `native_*` functions and
~304 `turi_env_register_native` calls still live in `src/main.c`**, spread across
17 `wk_register_*` wrapper functions plus three inline `contract` registrations.
`main.c` is compiled only into the `tur` executable, so none of these symbols
are in `libturi.a` / `libturi_wasm.a`:

- Wrapper definitions: `wk_register_stdlib_natives` (`src/main.c:8897`),
  `wk_register_seq_natives` (`:6849`), `wk_register_json_natives` (`:7316`),
  `wk_register_schema_natives` (`:7795`), `wk_register_sym_natives` (`:7870`),
  `wk_register_safe_natives` (`:9131`), `wk_register_comonad_natives` (`:9208`),
  `wk_register_mutex_natives` (`:9259`), `wk_register_future_natives` (`:9340`),
  `wk_register_chan_natives` (`:9481`), `wk_register_backtrack_natives`
  (`:9693`), `wk_register_proc_fs_natives` (`:9706`), `wk_register_serial_natives`
  (`:9761`), `wk_register_bytes_natives` (`:9801`), `wk_register_taskgroup_natives`
  (`:9878`), `wk_register_typeclass_natives` (`:9928`).
- Inline `contract` natives: `native_contract_check` / `_check_inv` /
  `contract_enabled` (`src/main.c:9951+`), registered directly (not via a
  wrapper) at both sites below.

There are **two** registration call sites in `main.c`, and they diverge:

1. `cmd_eval_h` -- the `tur --interpret` / `tur run` path (`src/main.c:5648-5698`)
   registers **all 17 wrappers + contract**.
2. `wk_eval_fixture` -- the fixture-runner path (`src/main.c:10010+`, calls at
   `:10106-10110`) registers **only** `contract` + `safe` + `typeclass` +
   `stdlib`.

So even inside the `tur` binary the fixture path is missing `seq`/`json`/
`schema`/`sym`/concurrency natives relative to the interpret path -- a latent
inconsistency this relocation can converge. The **WASM glue registers none** of
them (`src/web/wasm_glue.c` has no `wk_register_*` / `register_native` calls), so
the web REPL is as bare as any other embedder.

### Per-wrapper inventory (native count, backing surface, disposition)

| Wrapper | # regs | Backing stdlib surface | Disposition |
| --- | --- | --- | --- |
| `stdlib` | 137 | **mixed**: `time`/Mock-Time, `option`, `result`, `str`/int-conv, `math` (sqrt/floor/int<->float), `safe`-array, `slice`, typed-`list`, `reactor`, `free`, `grid`, `sized-buf`, `mutmap`, list ops -- **and** benchmark/fixture scaffolding (`run-ring`/`run-nbody`/`run-raytracer`, `io-*`, `random-access-bench`, `write-temp-file`, `flat-*`) | **split**: genuine stdlib -> libturi; benchmark scaffolding stays CLI-only |
| `seq` | 33 | `seq/*` lazy-Seq + generator bridges, `gen.tur`, `gen-arr` | move |
| `schema` | 31 | `schema.tur` runtime validator over JSON nodes | move (with `json`) |
| `json` | 21 | `json.tur` tagged-AST engine (encbuf + recursive decoder) | move |
| `chan` | 16 | `chan.tur` bounded sync/async channels | move |
| `backtrack` | 10 | `backtrack.tur` cons-stream monad | move |
| `comonad` | 9 | `comonad.tur` Identity/Pair cells | move |
| `sym` | 7 | `sym.tur` / `sym-dynamic.tur` interned symbols | move (reuses collections' `native_mk_cmp_int`/`native_mk_box_cstr`) |
| `typeclass` | 6 | Show/Eq inline-C instance overrides | move |
| `proc_fs` | 6 | `process.tur` spawn/wait + `fs.tur` tmpfile | move |
| `future` | 6 | `future.tur` refcounted FutureCell | move |
| `taskgroup` | 5 | `taskgroup.tur` TaskGroupBlock | move |
| `mutex` | 5 | `mutex.tur` pthread handles | move |
| `serial` | 4 | `serial.tur` Serializable int/bool | move |
| `safe` | 4 | `safe.tur` box/unbox/array-get/-set | move |
| `bytes` | 4 | `serial.tur` Bytes buffer | move |
| `contract` | 3 | `contract.tur` assert/require/ensure | move |

## Root cause

Identical in shape to the collections gap: the native overrides and their
registration are defined in `src/main.c` and invoked from the CLI command
handlers, not from `turi_env_new` / `turi_init`. Because `main.c` compiles only
into the `tur` executable
(`add_executable(tur main.c ... $<TARGET_OBJECTS:tur_core>)`), the symbols are
absent from `libturi.a` / `libturi_wasm.a`. The interpreter's
native-override-for-inline-C path (eval.c, the "keep native override" branch of
`EX_FN_DEF`) therefore finds no override for a `libturi` env, and the inline-C
body -- which the tree-walker cannot execute -- raises "inline-C not supported."

The runtimes these natives sit on are already in `libturi` (pthreads, `turi_call`,
`turi_gen_advance_val`, `Symbol`/`symtab`, `tur_panic`, `malloc`/`free`); the
blocker is purely *where the natives are defined and registered*.

## Goals

1. Every genuine stdlib inline-C op resolves to its native override for **any**
   interpreter env created through `libturi`, so the `tur` binary, embedders, the
   WASM REPL, and the test harnesses behave identically.
2. The two in-`main.c` registration sites (`cmd_eval_h`, `wk_eval_fixture`)
   converge on one code path, and the WASM glue picks the natives up with no
   per-consumer wiring.
3. No change to the compiled path, to the `tur` binary's observable behavior, or
   to native semantics -- this exposes existing natives to more callers.

## Non-goals

- **Benchmark / fixture scaffolding does not move.** `run-ring` / `run-nbody` /
  `run-raytracer`, the `io-*` file micro-ops, `random-access-bench`,
  `write-temp-file`, and the `flat-*` helpers are CLI/benchmark test harness, not
  a stdlib module an embedder loads. Moving them widens `libturi` (and the WASM
  build) for zero embedder benefit and drags in pthread/`fopen` benchmark code.
  They stay in `main.c` (or a clearly CLI-only `main.c`-compiled TU). The
  `stdlib` wrapper's split (below) is exactly this line.
- **No new natives, no representation changes, no leak fixes.** Same relocation-
  only contract as the collections plan. If an inline-C op has no native override
  even in the `tur` binary today, it stays unsupported (call it out, do not paper
  over it).
- **The `(load ...)` dependency-declaration work is separate.** This plan makes
  the *natives* available; a stdlib module still needs its elaboration deps in
  scope to `load` standalone (the `set.tur`/`map.tur` fix pattern). Apply that
  per-module `(load ...)` fix as embedders exercise each module -- it is a
  distinct, cheap follow-up, not gated here.

## Design

### Part 1 -- split the natives into domain TUs in `tur_core`

Relocate the native implementations + a per-domain registrar into new
`tur_core` translation units under `src/turi/`, added to `TURI_EVAL_SOURCES` in
`src/CMakeLists.txt` (so they land in `libturi.a` / `libturi_wasm.a`), following
`src/turi/collections_native.c` exactly. Group by cohesion and dependency, not
one monster file:

- `natives_core.c` -- `option`, `result`, `str`/int-conv, `math`, `safe`,
  `contract`, `comonad`, typed-`list`, `slice`, `reactor`, `free`, `grid`,
  `sized-buf`, `mutmap`, `typeclass` instance overrides. The high-value,
  low-risk, most-reused surface.
- `natives_seq.c` -- the `seq`/`gen` generator + fat-closure-call bridges.
- `natives_json.c` -- the `json` AST engine (`tur_json_encbuf` / `tur_json_ctx`
  + recursive encode/decode) **and** `schema` (schema sits directly on the JSON
  nodes; keep them together).
- `natives_concurrency.c` -- `mutex`, `future`, `chan`, `taskgroup`, `bytes`,
  `backtrack`, `proc_fs`, `serial` (the OS-handle + concurrency modules).
- `natives_sym.c` -- `sym` / `sym-dynamic`. Reuses collections' already-exported
  `native_mk_cmp_int` / `native_mk_box_cstr` (`turi/collections_native.h`) for
  `MapKey[Sym]` -- the cross-TU precedent already exists.

Each TU exports one registrar, e.g. `void turi_register_core_natives(TuriEnv *)`,
`turi_register_seq_natives`, `turi_register_json_natives`,
`turi_register_concurrency_natives`, `turi_register_sym_natives`, each doing the
same `turi_env_register_native(env, "<name>", <fn>, NULL)` sequence its
`wk_register_*` counterpart does today. Move whole functions **verbatim** (they
are self-contained -- sampled bodies touch only `TuriValue`, `malloc`/`free`,
pthreads, `turi_call`, and libturi runtime entry points; no `main.c`-local CLI
globals were found in `native_*` bodies). Pull the few co-located static helpers
along (`wk_int_ptr`, `json_enc_*`, `json_dec_*`, the seq closure/gen helpers);
audit each moved native for a stray `main.c`-local dependency and fix it as an
undefined symbol surfaces (the collections relocation hit exactly two --
`native_mk_cmp_int`/`native_mk_box_cstr` needed by `sym` -- resolved by exposing
them in the header).

**The `stdlib` wrapper split.** `wk_register_stdlib_natives` is the one mixed
bag. Partition it:
- genuine stdlib rows (`option`/`result`/`str`/`math`/`safe`/`slice`/`list`/
  `reactor`/`free`/`grid`/`sized-buf`/`mutmap`/`time`) -> `natives_core.c`;
- benchmark/scaffolding rows (`run-*`, `io-*`, `random-access-bench`,
  `write-temp-file`, `flat-*`) -> stay in `main.c` in a trimmed
  `wk_register_bench_natives`, still CLI-only.
  Some rows are dual-use (`sqrt`/`floor`/`int->float`/`cstr->parse-int` back real
  `math`/`str` ops) -- those move; only the benchmark harness stays.

### Part 2 -- register for every interpreter env

Add one umbrella `void turi_register_stdlib_natives_all(TuriEnv *env)` (name TBD;
avoid colliding with the existing `main.c` `wk_register_stdlib_natives`) in a
small `src/turi/natives_register.c` that calls each domain registrar. Call it
from `turi_env_new` (`src/turi/env.c`) right where
`turi_register_collection_natives(env)` is already called -- after
`turi_eval_register_builtins`, before `install_default_natives`, so an embedder-
seeded default of the same name still wins. Then:

- `main.c` drops the 16 relocated `wk_register_*` calls (and the inline
  `contract` registrations) from **both** `cmd_eval_h` and `wk_eval_fixture`,
  keeping only `wk_register_bench_natives`. The two sites converge on
  turi_env_new's auto-registration for everything real.
- The WASM REPL and every embedder pick the natives up automatically -- no
  `wasm_glue.c` change needed (matches how collections works).

**Registration-cost note.** ~280 name->fn-ptr inserts on every `turi_env_new` is
cheap (the collections precedent already added ~85). If profiling ever shows it
matters for short-lived envs, the domain registrars can be lazily de-duped
behind a one-time default-native seeding, but do not pre-optimize.

**Boundary note.** Auto-registering pulls `json`/`seq`/`schema`/concurrency code
into `libturi_wasm`. This is the same boundary-widening the collections plan
accepted; if the WASM binary size regresses unacceptably, the fallback is to
keep the umbrella registrar public and have `wasm_glue.c` call it explicitly
(opt-in, Part 2 option 2 from the collections plan) rather than auto-registering
in `turi_env_new`. Measure the `.wasm` delta before deciding.

### Part 3 -- converge and de-duplicate the CLI sites

With auto-registration in place, `cmd_eval_h` and `wk_eval_fixture` no longer
each hand-roll a (divergent) native list. This is a correctness win: fixtures
currently run without `seq`/`json`/`sym` natives, so a fixture that exercised
those under `--interpret` behaved differently in the two runners. Landing this,
watch for a fixture that *depended* on a native being absent (unlikely, but the
fixture suite is the gate).

## API / behavior changes

- New `tur_core` sources + public per-domain registrars and one umbrella
  `turi_register_stdlib_natives_all(TuriEnv *)` (or auto-registration inside
  `turi_env_new`). No change to `TuriValue`, `turi_eval*`, or the value-lifetime
  contract.
- `libturi` / `libturi_wasm` gain the stdlib native symbols; the `tur` binary's
  behavior is unchanged (same natives, registered through the new path;
  benchmark natives still CLI-only).
- The `wk_eval_fixture` path gains the `seq`/`json`/`schema`/`sym`/concurrency
  natives it lacked -- a convergence, verified against the suites.

## Batching / landing order

Land as a sequence of small, independently-reviewable PRs, each one TU + its
parity test, ordered by value-to-risk:

1. `natives_core.c` (option/result/list/str/math/safe/contract/comonad/
   typeclass) -- highest reuse, lowest risk; unblocks the most embedder code.
   Includes the `stdlib` wrapper split (benchmark rows stay behind).
2. `natives_seq.c`.
3. `natives_json.c` (json + schema).
4. `natives_concurrency.c` (mutex/future/chan/taskgroup/bytes/backtrack/proc_fs/
   serial).
5. `natives_sym.c`.

Wire `turi_register_stdlib_natives_all` incrementally: each PR adds its registrar
to the umbrella and turi_env_new. Each PR is green on its own (suites + its
parity test); no big-bang cutover.

## Testing

- **Per-batch embedding parity test**, extending the
  `tests/turi/collections-embed.c` pattern (linked against `libturi`, run through
  `turi_eval`, ctest target, leak detection matching the interpreter's
  process-lifetime policy): construct/drive each batch's ops purely through the
  embed API and assert read-backs. Each fails before its batch lands, passes
  after. Cover a `:float` where the op has a float carrier (retag path).
- **Fixture-path convergence**: confirm `wk_eval_fixture` gaining the extra
  natives changes no fixture result (`tests/run.sh`, `run-turi.sh`).
- **Regression**: full `tests/run.sh` (10-min timeout), `run-turi.sh`, and the
  `tur_env_teardown` / `tur_embed_peripherals` leak gates stay at baseline per
  batch. Watch for new leaks -- several natives are `malloc`-owned; free
  explicitly in the parity test or run it `detect_leaks=0` (interpreter policy).
- **WASM link + size**: confirm `libturi_wasm` still links and measure the
  `.wasm` size delta (Part 2 boundary note); confirm the web REPL resolves the
  new natives.
- **Browser smoke tests (Playwright, new).** Today the only signal that the
  `Try Turmeric` REPL works is a human reloading the tab and pasting console
  output -- the reason `#map{}` staying broken after fbf138669 went unnoticed
  until a user hit it, and the reason the `if`-in-`definstance` hang
  ([`docs/reported/wasm-interp-hang-if-in-definstance-bool.md`](../reported/wasm-interp-hang-if-in-definstance-bool.md))
  and the `set.tur` OOB
  ([`docs/reported/wasm-interp-set-tur-oob-trap.md`](../reported/wasm-interp-set-tur-oob-trap.md))
  both hid behind a silent `_turi_wasm_init` hang. Scaffold `web/tests/e2e/`
  with Playwright (Chromium already honors the Vite dev server's COOP/COEP
  headers, so `SharedArrayBuffer` / the pthreads worker just work):

  Batch 0 (do first, before any relocation batch lands):
  - `pnpm add -D @playwright/test` in `web/`; commit `playwright.config.ts`
    that boots `vite preview` on a random port with the COOP/COEP headers.
  - `smoke.spec.ts` -- one spec covering: navigate to `/try/`, wait for
    `#wasm-status-text` to read `Ready` (or the loading overlay to hide),
    evaluate `(+ 1 2)` -> `3`, `#map{}` -> a HAMT handle, `(println "hi")`
    -> `hi` in the console pane. Set the Monaco editor via
    `page.evaluate((v) => monaco.editor.getEditors()[0].setValue(v), src)`,
    click the Run button, and read the REPL output pane.
  - Wire into `tur run test` (or a new `tur run test-web` task) and add a CI
    job. Cache the Playwright browser download so CI cost stays flat.

  Batch N testing gate (repeat per relocation batch): the smoke spec is the
  fastest signal that a moved native didn't break the browser path. Add a
  batch-specific case only when the batch exposes a new op the smoke spec
  can't already reach transitively (rare -- `hamt-of` covers most of the
  collection surface).

  This is scoped as a scaffold, not a full e2e suite. Coverage grows as the
  known-broken paths (the two reported hangs, plus any that surface as
  natives move) get fixed and become regressable.

## Risks

- **Hidden `main.c` coupling.** A moved native may lean on a `main.c`-local
  static or global. Mitigate exactly as collections did: compile the new TU
  early, fix each undefined-symbol / shared-helper as it surfaces, keep CLI-only
  helpers behind. Sampling found none in `native_*` bodies, but the JSON/seq
  helpers are the most intertwined -- audit those first.
- **The `stdlib` split is the fiddly part.** Getting the benchmark-vs-genuine
  line right (dual-use rows like `sqrt`/`cstr->parse-int`) needs care; a wrong
  cut either drags benchmark code into `libturi` or drops a real stdlib op.
- **Boundary / size creep in WASM.** Auto-registering the JSON engine + seq +
  concurrency into `libturi_wasm` grows the web bundle. Measure; fall back to the
  explicit opt-in registrar for WASM if needed.
- **Fixture-path convergence surprises.** Giving `wk_eval_fixture` the full
  native set could change a fixture that implicitly relied on a native's absence.
  The suites are the gate; investigate any diff rather than force it.
- **OS-handle natives in a bare embedder.** `proc_fs`/`serial`/`chan`/`mutex`
  natives spawn processes / open fds / create threads. Registering them for every
  env only makes them *callable*; they fire only when the user's code calls them,
  so this does not change sandbox posture. If an embedder wants them gated, that
  is a capability question (`TuriEnv.caps`), out of scope here.

## Recommendation

Do batch 1 (`natives_core.c` + the `stdlib` split) first: it is the highest-value
slice (option/result/list/str/math back the most embedder code), it exercises the
whole mechanism (new TU, umbrella registrar, turi_env_new wiring, CLI-site
convergence, parity test), and it is mechanically the collections relocation
again. Land the remaining batches on the same rails, one TU per PR, measuring the
WASM size after the `json`/`seq`/`concurrency` batches. Keep benchmark
scaffolding in `main.c` throughout -- it is not stdlib and does not belong in
`libturi`.
