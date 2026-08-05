# Fat-closure env leak when the enclosing `let` also binds an owning value

**Status: RESOLVED (2026-07-20).** Distinct from (but adjacent to) the still-open
`docs/reported/escaping-fat-closure-env-leak.md`: that report is about a closure
that ESCAPES its constructor (returned / stored / passed `^fat`); this is a
NON-escaping closure whose env-free was suppressed by an unrelated owning sibling
binding.

## Symptom

A non-escaping capturing closure -- exactly the shape the existing
`let_binding_env_freeable` scope-exit free is meant to reclaim -- leaked its 16 B
heap env whenever the enclosing `let` ALSO bound an owning value (`rc`/`ref`),
even when the closure captured only scalars:

```turmeric
(defn compute [] : int
  (let [r     (rc/of 100)                       ;; owning sibling binding
        k     5
        scale (fn [x : int] : int (+ x k))]     ;; non-escaping, scalar capture
    (scale 10)))                                ;; scale's env leaked (16 B)
```

Remove the `r` binding and the env is freed (clean); add any `rc`/`ref` sibling
and it leaks. Invisible to `bash tests/run.sh` (the harness compiles emitted
programs without `-fsanitize=address`), so no suite gate caught it.

## Root cause

`binding_escapes_impl` (`src/compiler/emit_core.c`) decides whether a closure
binding `b` escapes its scope; `let_binding_env_freeable` only frees the env when
it does not. The analysis has a conservative `default: escape` arm (sound
posture: never miss an escape). Two node kinds that an owning binding introduces
hit that arm:

1. **The owning binding's auto-drop.** An `rc`/`ref` let-binding lowers its
   scope-exit drop to a `(defer (drop r))` injected into the let body. `EX_DEFER`
   was unmodeled -> `default: escape` -> the body was read as an escape of *every*
   sibling closure.
2. **The owning binding's initializer.** `(rc/of ...)` is an `EX_RC_OF` node (and
   the rest of the rc/weak family likewise); these were unmodeled in
   `binding_escapes_impl` -> `default: escape` -> the sibling-init escape check
   flagged every sibling closure.

Either alone was enough to suppress the free.

## Fix

Model both precisely instead of blanket-escaping (`src/compiler/emit_core.c`):

- **`EX_DEFER`**: consult the defer's precomputed `captures` set (mirroring
  `EX_CLOSURE`/`EX_FN_DEF`). A defer runs at scope exit, but can only reach `b`
  through its captures -- so it is an escape only when it actually captures `b`
  (e.g. `(defer (use b))`), never for an unrelated `(defer (drop r))`.
- **rc/weak/ref family** (`EX_RC_OF`, `EX_RC_CLONE`, `EX_RC_DROP`, `EX_RC_PTR`,
  `EX_RC_COUNT`, `EX_RC_FROM_REF`, `EX_REF_FROM_RC`, `EX_WEAK`, `EX_WEAK_UPGRADE`,
  `EX_WEAK_PRED`, `EX_REF_PRED`): single-operand nodes -- walk the operand so `b`
  is detected iff it flows into one (`(rc/of b)` stores the closure into an rc ->
  correctly an escape), rather than defaulting to escape.

Both changes only ever make the analysis MORE precise (fewer false escapes); they
never greenlight a free of an env that a defer/rc-op actually references, so the
"only ever greenlight a safe free" soundness posture is preserved.

## Verification

- The repro above and variants (scalar capture, rc capture, 3-way) drop to **0
  leaked, no double-free** under ASan/LSan+UBSan.
- `(rc/of f)` (wrapping the closure itself) correctly STILL does not free -- `f`
  genuinely escapes into the rc; that residual is the separate escaping-closure
  leak (S2), untouched.
- Regression fixture: `tests/fixtures/closure-env-free-with-owning-sibling/`.
- Full suite `2215 passed, 0 failed`, no snapshot churn.

## Out of scope (still open)

The `escaping-fat-closure-env-leak.md` headline repro (`make-scaler` returned
then consumed by `use-it`) and the `requires.no-leak-check` fixtures
(`cps-backend-fn-param`, `free-lift-bind`, `unsafe-closure-capture`) are the
ESCAPING / inline-HOF-arg cases -- a different leak source that needs the
ownership feature (closure-drop-glue-plan S1c / S2). They still leak; that report
stays open.
