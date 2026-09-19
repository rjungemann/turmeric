# `web/examples.js` is not used by the playground, and most of its examples no longer run

**Severity:** low (dead code that misleads: reports cite it as the playground's
examples).

**Status: RESOLVED 2026-09-19.** `web/examples.js` is deleted and the
`web/README.md` project-structure tree no longer lists it. The one list the
page reads -- `EXAMPLES` in `web/main.js` -- is the only one left; nothing
imported the deleted file (checked: no `import`, no `<script>`, no test
reference), so no example the page offered has changed.

Filed 2026-09-16 while executing
[playground-session-hygiene-plan](../archive/playground-session-hygiene-plan.md),
whose source report cited `web/examples.js:99` as "the effects entry ...
reachable straight from the examples dropdown".

## What is true

- The page's examples dropdown reads the `EXAMPLES` object defined inside
  `web/main.js` (seven entries: hello, math, factorial, fibonacci, closure,
  effects, sweet). Nothing imports `web/examples.js`.
- `web/examples.js` exports a different, larger `EXAMPLES`. Run through the
  playground's own eval path (`libturi_wasm`, one session), 7 of its 11
  examples fail on the FIRST run: `math`, `matching`, `vectors`,
  `higher_order` with parse errors (`%`, `**`, ...), `rc` and `types` with
  "unknown function or operator 'str'", and `effects` with
  `operator lookup failed for '+': got 4 arg(s), first arg type cstr` plus
  "handle expects pairs of (case-header body)".
- All seven `main.js` examples run, and (with PS1-PS5) run identically twice.

## Fix directions

Delete `web/examples.js`, or port its working examples into `main.js` and
delete it -- either way there should be one list, and it should be the one
the page reads.
