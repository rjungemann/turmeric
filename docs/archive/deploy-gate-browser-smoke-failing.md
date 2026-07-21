# `Try Turmeric` deploy-gate browser smoke test failing in CI

**Status: RESOLVED (2026-07-21).** Same root cause as
[tur-eval-prints-fn-main-no-eval](../archive/tur-eval-prints-fn-main-no-eval.md):
the top-level statement -> synthesized-`main` fold in
`src/compiler/elab_toplevel.c` (graduated to default-on with the
`cps-tramp-resume` experiment on 2026-07-19) also fires on the WASM interpreter
path (`turi_wasm_eval` -> `turi_eval_typed` -> `elaborate_program`). The
deploy-gate test types `(+ 1 2)` and expects `3`; the fold instead wrapped it
into `(defn main [] : int (do (+ 1 2) 0))`, so `turi_wasm_eval` returned the
synthesized closure `#<fn main>` and the `#console` never contained `3` --
exactly the assertion that tripped (`deploy-gate.spec.js:37`). The
`TUR-W0040 unknown name 'when'` warnings from `typeclass-show.tur` in the same
console output are a **pre-existing cosmetic red herring** (they reproduce under
native `tur repl` too, which still returns `3`).

**Fix:** commit `d95d8a57a` (2026-07-21, in PR #699) gated the fold on
`!g_interpret_mode`, so the interpreter (native and WASM) evaluates top-level
forms directly. **Verified:** a fresh WASM build from HEAD, driven through a
Node harness, evaluates `(+ 1 2)` -> `3` and `(println "hi")` -> `nil` (the
stale pre-fix build returned `#<fn main>` for both). CI confirms the
`Try Turmeric smoke test (browser)` job is green on both post-fix runs
(`d95d8a57a`, 4m4s; `c5ff99f90`/HEAD, 4m30s). The last *red* completed run
(`35635cb06`, "Doc stuff") predates the fix. No web-side change was needed --
the WASM `tur_wasm` target already `copy_if_different`s its fresh build into
`web/public/`, so CI was serving correctly-built (but pre-fix) WASM.

---

**Severity:** medium (CI-only; blocks the `Try Turmeric smoke test (browser)`
job on every push/PR). ~~Root cause **not yet pinned** -- needs the uploaded
Playwright report artifact to diagnose.~~ Pre-existing on `main`, independent of
any one PR.

**One-line:** the single deploy-gate Playwright test
(`web/tests/deploy-gate.spec.js:14` -- "Try Turmeric loads and evaluates a basic
expression") fails, so the browser smoke job is red. All build steps before it
(native `tur`, gendocs, WASM configure+build, web deps, Playwright install)
succeed; only the test assertion fails.

## Repro

CI: run #2015 (PR) and run #1999 (`main`), job `Try Turmeric smoke test
(browser)` -> step "Run deploy-gate smoke test":

```
1 failed
  [desktop] > tests/deploy-gate.spec.js:14:1 > Try Turmeric loads and evaluates a basic expression
##[error]Process completed with exit code 1.
```

A `playwright-report` artifact is uploaded on failure (e.g. run #2015 artifact
ID 8481985491) -- that HTML report has the actual assertion + trace and is the
fastest path to root cause.

Local repro (not yet run): build WASM + serve `web/`, then
`cd web && npx playwright test tests/deploy-gate.spec.js`.

## What the test asserts (surfaces to check)

From `web/tests/deploy-gate.spec.js`:

1. `page.goto('/try/')` then wait up to 30s for `#wasm-status-text` to read
   `Ready` -- i.e. the eval Worker posts `ready` and `main.js` sets
   `wasmState = READY`. A stalled `_turi_wasm_init` / broken preload chain fails
   here.
2. Set the Monaco editor to `(+ 1 2)` via `window._turiEditor`, click
   `#run-btn`, expect `#console` to contain `3` within 10s and no
   `.console-error` span.

So the failure is one of: (a) WASM never reaches `Ready` (init/preload
regression), (b) eval produces the wrong text or an evaluator-error span, or
(c) an uncaught JS `pageerror`/console error (the spec also collects those).
The report artifact will say which.

## Fix direction

Pull the Playwright report artifact from a failed run, identify which assertion
tripped, and trace back:
- If `#wasm-status-text` never hits `Ready`: inspect the WASM preload/worker
  init chain in `web/` (`main.js`, the eval worker, `web/turmeric.js`).
- If eval mismatch / error span: check the WASM `turi` eval path for `(+ 1 2)`.
- If a JS pageerror: read the collected `jsErrors` in the report.

Note the WASM build embeds the same DK/CPS runtime prelude as native builds, so
confirm whether this predates recent CPS codegen changes by checking older
`main` runs (it is already red on run #1999, so it predates the
`cps-reopen-perform-onode-leak` PR).

Discovered while triaging CI on the `cps-reopen-perform-onode-leak` PR.
