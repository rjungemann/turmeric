# defimage-global registry + TUR-W0706 lint (plan AI3) unimplemented -- mutable globals silently fall out of image dumps

**Severity: medium** -- a silent-data-loss foot-gun the guide already warns
about: the workaround is threading state through the captured continuation.
Found in the 2026-08-20 docs audit.

## Repro

`grep -rn "defimage-global\|W0706" src/ stdlib/` -> nothing. A `def ^mut`
written during init is absent after a warm `load-image!`.

## Root cause / tracking

docs/archive/history/application-image-dumps-plan.md phase AI3, unbuilt.

## Fix direction

Per the plan: a registration form that serializes declared globals alongside
the continuation in the TSER payload, plus the lint for unregistered mutation
reachable from a cache body.

## Guides to update when fixed

- docs/guides/image-dumps-guide.md ("Globals" section and See-also)
