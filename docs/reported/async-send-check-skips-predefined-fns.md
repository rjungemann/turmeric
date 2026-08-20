# Send-across-await check (TUR-E0022) skips pre-defined fns passed to (async fn)

**Severity: medium** -- a documented soundness hole: a non-Send value (e.g.
`rc<T>`) can be live across an await with no diagnostic when the async body is
a pre-defined function rather than an inline closure. Found in the 2026-08-20
docs audit.

## Repro

```turmeric
(defn f [] : int
  (let [x (rc/of 1)]
    (await (async g))
    (rc/deref x)))
(async f)  ;; no diagnostic
```

## Root cause

src/compiler/elab_concurrent.c:146 (`if (e->in_async_body)`) -- the check runs
only inside inline `(async (fn [] ...))` closures; the comment says
"pre-defined functions passed to (async fn) are not re-elaborated here".

## Fix direction

Now that cps-async lowering is unconditional (fixture `async-await-cps`,
graduated 2026-07-19), run the Send check on any CPS-colored defn body at its
await points.

## Guides to update when fixed

- docs/guides/async-await-guide.md (Scope subsection)
