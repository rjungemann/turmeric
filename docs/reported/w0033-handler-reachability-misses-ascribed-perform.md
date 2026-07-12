# TUR-W0033 handler-reachability misses a `perform` hidden inside a type ascription (false "unreachable clause")

**Severity: LOW (spurious warning, no miscompile -- the handler runs correctly;
the diagnostic is just a false positive). Sibling of the now-fixed CPS-coloring
ascription-descent gap
([docs/archive/cps-coloring-ascription-hides-control-op.md](../archive/cps-coloring-ascription-hides-control-op.md)),
in a different analysis pass.**

## Summary

The reachability check behind `TUR-W0033` ("handler clause for `E` is
unreachable: the body does not perform `E`") does not descend into type
ascriptions when scanning a handled body for `perform`s. So when the only
`perform E` in the handled expression sits inside a `(:: (perform (E ...)) T)`
ascription, the analysis concludes the body never performs `E` and warns that
the handler clause is unreachable -- even though the clause is reached and runs
at runtime.

This is the same root-cause class as the CPS-coloring gap just fixed in
`src/passes/cps.c` (which had no `EX_ASCRIBE` case and a non-descending
`default`), but it lives in the separate handler/effect-row reachability pass
that emits W0033, so the coloring fix does not address it.

## Minimal repro

```turmeric
(defeffect Ask [] :int)

(defn use-ask [] : int
  (+ 1 (:: (perform (Ask)) :int)))   ;; the ONLY perform is inside the ascription

(defn run [] : int
  (handle (use-ask)
    (Ask [] k) (resume k 41)))       ;; <- W0033 fires here, wrongly

(defn main [] : int
  (println (run))                    ;; prints 42 -- the clause DID run
  0)
```

```
$ ./build/tur run repro.tur
repro.tur:7:16: warning [TUR-W0033]: handler clause for 'Ask' is unreachable: the body does not perform 'Ask'
...
42
```

The program is correct (`42`); the warning is spurious. Removing the ascription
(`(+ 1 (perform (Ask)))`) silences it, confirming the ascription is what hides
the `perform` from the scan.

## Root cause

The W0033 emitter walks the handled body looking for a `perform` of the
handler's effect and, like the pre-fix coloring traversals, has no `EX_ASCRIBE`
case, so it does not recurse into `ascribe_.inner`. Ascription is erased at
codegen (`emit_expr.c` unwraps `ascribe_.inner`), so the runtime behavior is
correct; only the static reachability scan is fooled. The exact site is the
handler-clause reachability analysis (grep for the `W0033` / "is unreachable"
string and the effect-row/handled-body `perform` walk -- likely in the effects
elaboration or a diagnostics pass, not `src/passes/cps.c`).

## Fix directions

- Add an `EX_ASCRIBE` case to the W0033 body-scan that recurses into
  `ascribe_.inner` (mirroring the fix applied to `cps_directly_uses_control` /
  `cps_expr_contains_shift` / `cps_collect_calls` in `src/passes/cps.c`). Audit
  the same walk for other transparent wrappers it may also be failing to
  descend through.
- Add a fixture asserting the repro compiles with **no** W0033 on stderr (the
  existing `tests/fixtures/cps-backend-ascribe-only-control/` already exercises
  this shape and currently tolerates the warning; tightening it to assert the
  warning's *absence* would double as the regression guard once fixed).

## Scope

Surfaced while landing the CPS-coloring ascription fix: the shared regression
fixture prints the W0033 warning, which made the sibling gap visible. Narrow and
self-contained; a good follow-up to the coloring fix since it is the same
missing-`EX_ASCRIBE`-descent bug in a second pass.
