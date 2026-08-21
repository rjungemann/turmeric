# Send-across-await check (TUR-E0022) skips pre-defined fns passed to (async fn)

**Severity: medium** -- a documented soundness hole. Found in the 2026-08-20
docs audit.
**Status: RESOLVED** -- the check runs at every await point.

## Repro (verified on `main` before the fix)

```turmeric
(load "stdlib/rc.tur")
(defn zero [] : int 0)

(defn f [] : int
  (let [x (rc/of 42)]      ;; rc<int> -- not Send
    (await (async zero))   ;; x live across the await
    (rc/deref x)))

(defn main [] : nil (await (async f)))
```

Compiled clean. The identical body written inline as
`(async (fn [] ...))` was a `TUR-E0022` error. So the diagnostic was evadable
by hoisting: moving the body one line up into a named `defn` silenced a
soundness check.

## Root cause

src/compiler/elab_concurrent.c -- `elab_await`'s check was gated on
`e->in_async_body`, a flag set only while the elaborator walked *inside* an
`(async ...)` form. A pre-defined function is elaborated at its own definition
site and never re-elaborated there, so its awaits were never checked. The
comment said as much: *"pre-defined functions passed to (async fn) are not
re-elaborated here."*

## Resolution

The gate is removed: the check runs at **every** `await`. cps-async lowering
is unconditional now (fixture `async-await-cps`, graduated 2026-07-19), so a
body containing an await is fiber-resumable regardless of how it reaches
`async` -- the hazard does not depend on the syntactic form the elaborator
happens to be standing in.

`in_async_body` had no remaining reader afterwards, so it is deleted from
`Elab` along with its set/restore pair in `elab_async`, rather than left
set-but-never-read.

## Blast radius

**Zero.** The unconditional check was measured before being written up:
`run.sh` 2669 passed / 0 failed and `run-turi.sh` 1840 passed / 0 failed with
the gate forced open. Nothing in the corpus was relying on the hole.

## Tests

`tests/fixtures/errors/await-live-not-send-predefined-fn` -- the repro above,
deliberately the same body as its sibling `await-live-not-send` with only the
inline-vs-hoisted difference, so the pair pins that the two spellings are
diagnosed alike.

## Guide updated

docs/guides/async-await-guide.md "Scope" subsection said the check applied
"only to inline closures" and that pre-defined functions "are not checked
here". It now states that the check runs at every await, shows both spellings
being rejected, and gives the actual way to carry a non-Send value near an
await: end its scope before the await point with a nested `let`. That escape
route was verified to compile, not assumed.
