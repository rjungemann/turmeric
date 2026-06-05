---
title: Still-in-Flight Plans -- Outstanding Tasks
category: Planning
description: Consolidated outstanding work from in-flight plans that survived the post-v0.18.0 archive churn. Each section is a short punch list with a link back to the full plan in docs/archive/.
---

# Still-in-Flight Plans -- Outstanding Tasks

Each section below tracks one plan that is still active after the
post-v0.18.0 archive sweep. The full design lives in
`docs/archive/`; the bullets here are just the remaining work.

When a section's punch list is empty, move the plan from
`docs/archive/` to `docs/archive/history/` (per the churn-docs skill)
and delete the section from this file.

---

## Closure representation unification

> Full plan: [../archive/closure-representation-unification-plan.md](../archive/closure-representation-unification-plan.md)

All three phases are now executed; the punch list is empty and the
plan is ready to move to `docs/archive/history/`.

- [x] Phase 1 -- stdlib thin-call consumers migrated off the bare
      int64 thin cast: `arrow`/`option-map`/`pair`/`mutmap` use `^fat`
      + fat dispatch; `comonad` `extend` takes its function as a `:fn`
      poly-closure carrier and applies it directly (a `:fn` typeclass
      method param is the right surface for a dispatch-erased receiver,
      where `^fat` does not thread). Fixing comonad surfaced and
      resolved a closure-carrier miscompile -- see
      [poly-wrap-of-capturing-closure-value-references-local-env.md](../reported/poly-wrap-of-capturing-closure-value-references-local-env.md).
- [x] Phase 2 -- the nullary `:ptr<void>` direct-call path already
      fat-dispatches when the sink is `is_fat` (CRU B-2: the `n == 0`
      branch in `emit_expr.c` gates on `!fn_binding->is_fat`, so a fat
      sink falls through to slot-0 dispatch while a raw C callback
      stays thin).
- [x] Phase 3 -- Option B (first-class closure type) shipped;
      captureless fns are boxed at fat-dispatched sinks and raw
      C-callbacks (`contract.tur`) keep the bare representation.
- [x] Capturing-closure fixtures: `comonad-capturing-closure` and
      `poly-fn-typeclass-capturing-closure` (`:int`); `arrow`/`option`
      capturing + `:float` covered by `float-fat-closure`,
      `arrow-capturing-closure`, `eq-carrier-capturing-comparator`,
      etc. (comonad is an int64-carrier comonad; its `:fn` carrier is
      the int register class by design). `contract.tur` C-callback path
      still works.
- [x] `bash tests/run.sh` zero `FAIL` with leak detection on
      (`1482 passed, 0 failed`).

## Typed closure invocation ABI

> Full plan: [../archive/closure-typed-invocation-abi-plan.md](../archive/closure-typed-invocation-abi-plan.md)

Companion to the representation-unification plan; threads declared
arg/return types all the way to the C invocation site.

- [x] Phase 1 -- `TUR_APPLY{0..4}_T(R, A_i..., f, args...)` macros
      emitted (`emit_module.c`); legacy `TUR_APPLY{0..4}` are int64
      aliases of the `_T` forms; signature-keyed typed fat-shims
      (`ensure_typed_fatshim`) and the typed `__tur_poly_to_fat`
      generalisation (`ensure_typed_poly_to_fat`) land.
- [x] Phase 2 -- inline-C lowering, typeclass dispatch, and
      `EX_FN_TO_FAT`/`EX_POLY_TO_FAT` emit typed calls keyed off
      `result_full_type` + `param_types` via
      `ensure_typed_thunk_typedef` / `use_typed_thunk_abi`.
- [x] Phase 3 -- stdlib migrations: `httpd.tur` `mw-cors` (`^fat`)
      and `list.tur` `__cons-fmap` (`^fat` + direct call) done; the
      `seq-call-fn{0,1,2}` / `seq-call-bool-fn1` helpers (in
      `stdlib/seq/`, not `select.tur`) now dispatch through
      `TUR_APPLY0/1/2` and `TUR_APPLY1_T(bool, ...)` instead of
      bespoke slot-0 casts.
- [ ] Phase 4 -- migrate `turmeric-spices/signal` (`__signal_call1`
      returns `:float` and drops `int64_t sig_val; memcpy(...)`
      blocks). Sibling-repo work; deferred until the
      `../turmeric-spices` checkout is present.
- [x] Round-trip fixtures cover `:float`, `:ptr`, `:bool` returns and
      a `:cstr` argument (`tur-apply-t-fatshim-float`,
      `poly-to-fat-float-roundtrip`, `bare-fat-float-result`,
      `option-result-c-abi`, ...).

## `defmodule` per-file scoping

> Full plan: [../archive/defmodule-per-file-scoping-plan.md](../archive/defmodule-per-file-scoping-plan.md)

The `tur/zlib` workaround shipped (M6 dropped `defmodule`); the
diagnostic-scope fix itself is not implemented.

- [ ] D0 -- happy fixture `tests/fixtures/elab-defmodule-after-load/`
      + negative fixture `tests/fixtures/errors/elab-defmodule-not-first/`.
- [ ] D1 -- track per-file form boundaries through pass 1 (side
      array `file_id[nforms]`).
- [ ] D2 -- rewrite the check at `elab_toplevel.c:896-912` to
      operate per file (continue past the first defmodule; flag
      misplaced defmodules in later loaded files).
- [ ] D3 -- generalise the M7 `e.has_defmodule = false` reset to
      fire at every user-side file boundary, not just stdlib.
- [ ] D4 -- restore `(defmodule tur/zlib ...)` in
      `../turmeric-spices/spices/zlib/src/tur/zlib.tur`; verify
      `tests/fixtures/httpd-mw-compress/` and the spice roundtrip
      test still compile.
- [ ] D5 -- single-paragraph correction wherever `defmodule`'s "must
      be the first form" rule is documented.

## Drop leading colons inside `(fn ...)` types

> Full plan: [../archive/fn-type-bare-identifier-plan.md](../archive/fn-type-bare-identifier-plan.md)

Outer-annotation analogue ([spaced-type-annotation-migration-plan](#spaced-type-annotation-migration))
is complete; this is the inner-type-expression version.

- [x] Phase 1 -- parser accepts bare-identifier types inside
      `(fn ...)` everywhere; regression fixture
      `tests/fixtures/fn-type-bare-identifier/` covers unary,
      binary, nullary, curried, mixed, parenthesised inner, declared
      user type. (Fixture present and green in `tests/run.sh`.)
- [~] Phase 2 -- `tools/rewrite_fn_type_colons.py` codemod written
      (position-driven: defn/fn params + return, defstruct fields,
      let bindings, defalias, method sigs, `(:: _ T)`/`(the T _)`
      ascriptions, nested `(fn ...)`; handles `:kw`, bare `: (T)`,
      and effect sets; never touches lambda values). Corpus regression
      harness at `tests/codemod/run-fn-type-colons.sh`. **Swept so far:**
      `stdlib/` (`either`, `gadt-vec`, `list`) and `docs/guides/`.
      **Remaining:** `tests/fixtures/` sources are intentionally left
      on the legacy colon spelling until Phase 4 (they back the
      lenient-path coverage), and `../turmeric-spices/` is deferred
      until that checkout is present. Full suite stays green
      (`1479 passed, 0 failed`); the rewrite is annotation-spelling
      only, so codegen snapshots are byte-identical. **Known gap:**
      the codemod only finds fn types inside paren-delimited
      declarations, so sweet-exp top-level forms (`.tur.sweet` files,
      ```sweet-exp doc blocks) are skipped -- port the
      `spaced-types-rewrite.py` implicit-sequence walk before sweeping
      those.
- [ ] Phase 3 -- emit `TUR-D000x` deprecation warning when an
      `F_KEYWORD` is consumed inside a `(fn ...)` type position.
- [ ] Phase 4 -- remove the `F_KEYWORD` branch from the
      `(fn ...)` type-expression handler; convert the lenient-path
      fixture into an `errors/` fixture asserting the hard
      diagnostic.

## HTTPD compression + `tur/zlib` spice

> Full plan: [../archive/httpd-compression-zlib-spice-plan.md](../archive/httpd-compression-zlib-spice-plan.md)

Track Z (spice) and Track M6 (`stdlib/httpd-compress.tur`) both
unstarted.

- [ ] Z0 -- scaffold `../turmeric-spices/spices/zlib/` mirroring the
      `png`/`json` layout; pin upstream zlib via `:cmake-deps`.
- [ ] Z1 -- `src/zlib.tur` exposes `gzip-encode` / `gzip-decode` /
      `deflate-raw` / `inflate-raw` + `GzipBuf` accessors over
      `:ptr<void>` (binary-safe).
- [ ] Z2 -- `tests/zlib-roundtrip/` fixture (encode large input,
      decode, byte-for-byte equality).
- [ ] Z3 -- spice README.
- [ ] M6-0 -- `stdlib/httpd-compress.tur` scaffold loading
      `tur/zlib` with a clear load-time error if the spice is
      missing.
- [ ] M6-1 -- `mw-compress` factory: parse `Accept-Encoding`, gate
      on `min-bytes` threshold (default 256; revisit per OQ3),
      compress + set `Content-Encoding`.
- [ ] M6-2 -- spice-gated fixture (`requires.spices` marker).
- [ ] M6-3 -- brief docs entry.
- [ ] Resolve OQs: `(load "tur/zlib")` vs `(import ...)` spice
      form (M6-OQ1); `httpd-resp-body-bytes!` setter vs
      length-prefixed `httpd-resp-body!` (M6-OQ2); default
      `min-bytes` threshold (M6-OQ3).

## `tur-signal` spice broken build

> Full plan: [../archive/signal-spice-broken-build.md](../archive/signal-spice-broken-build.md)

Spice does not currently compile; Phase 0 of the expansion plan
needs to be split.

- [ ] Phase 0a -- restore the build. Either reintroduce
      `__arrow_call1` as a stdlib-side fat-dispatch shim, or rewrite
      every call site to direct closure invocation `(sv t)` plus
      `^fat` annotation; add missing `(import ...)` forms to
      `synth.tur` and `tests/signal/arrow_tests.tur`. Validation:
      `./build/tur check` clean on every spice source + test file.
- [ ] Phase 0b -- closure-ABI cleanup: replace raw
      `int64_t(*)(int64_t)` inline-C casts in `dsp.tur` with
      fat-dispatch helpers (`TUR_APPLY1` or typed Turmeric
      trampoline).
- [ ] Phase 0c -- `:float` sample migration (the originally-named
      Phase 0). Gate on compiler support for `:float`-returning
      closures through fat dispatch.
- [ ] Confirm DSP filter tests (`low-pass` / `high-pass` / `gain`)
      run a multi-sample sequence rather than a single dispatch.

## Spaced-type annotation migration

> Full plan: [../archive/spaced-type-annotation-migration-plan.md](../archive/spaced-type-annotation-migration-plan.md)

Phases 1-5 are complete (named-let, let/let*, codemod, repo + spices
rewrite, docs). Only enforcement and optional deprecation remain.

- [ ] Phase 6 -- add a CI step running
      `tools/spaced-types-rewrite.py --check` over the repo. Fused
      annotations in `.tur` / `.tur.sweet` cause CI failure. Mirror
      the hook in `../turmeric-spices`. Optionally lift the codemod
      into `tur fmt`.
- [ ] Phase 7 (optional, deferred) -- once the ecosystem is fully
      migrated and CI prevents regressions, emit a deprecation
      warning for fused `F_KEYWORD` in recognised type-annotation
      positions. Not committed; revisit after several release
      cycles.

## Stdlib opaque handle types

> Full plan: [../archive/stdlib-opaque-handle-types-plan.md](../archive/stdlib-opaque-handle-types-plan.md)

Tier 1 (threadpool, future, chan), Tier 2 (timer, reactor,
taskgroup, mutex/condvar/rwlock), and Tier 3 (atomic, stm, thread,
fiber + I/O fd sweep + process Pid + fs StatInfo/TmpFile + io
DirListing/FileSystem/FileStream + ref RefHandle) have all landed.
Only the tail items remain.

- [x] `io/file-open` -- parameters are annotated
      `[path : cstr mode : cstr]` (`stdlib/io.tur:368`); the linear
      `FileHandle` open path accepts a `:cstr` path as written.
      Verified in-tree -- no further change needed.
- [x] Final acceptance pass: `tests/run.sh` is green
      (`1479 passed, 0 failed`) with all Tier 1+2+3 modules exposing
      handle-typed signatures. No remaining tail items; this section's
      punch list is now empty and the plan is ready to move to
      `docs/archive/history/` (per the churn-docs skill).

When this list is empty, archive the plan to `docs/archive/history/`
under the post-v0.18.0 sweep.
