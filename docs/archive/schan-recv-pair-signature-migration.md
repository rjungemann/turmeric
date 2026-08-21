# schan-recv still delivers via a caller-provided cell although its blocker is fixed

**Severity: low** (cleanup/expressiveness). Found in the 2026-08-20 docs audit.
**Status: RESOLVED** -- `schan-recv` returns `(Pair T (SChan R))`.

## Repro

stdlib/schan.tur:149 --
`(defn schan-recv [T R] [c : (SChan (SRecv T R)) cell : ptr<void>] : (SChan R))`

## Root cause

A workaround for the generic-struct-opaque-element miscompile: a generic
function could not return a parametric aggregate whose element type is a
phantom carried only inside an opaque argument -- the type variable was not
recoverable from the opaque, so the aggregate result never specialized and the
carrier fallback miscompiled. Both variants were fixed 2026-06-05
(docs/archive/history/generic-struct-opaque-element-miscompile.md, pinned by
tests/fixtures/generic-relay-aggregate-result). The workaround outlived it.

Confirmed still fixed before starting, rather than taken on the report's word:
a generic `(defn split [T] [v : T h : Handle] : (Pair T Handle))` over a
`defopaque` compiles and runs.

## Resolution

```turmeric
schan-recv : SChan<SRecv T R> -> Pair<T SChan<R>>
```

Built in **ordinary Turmeric** over two inline-C leaves:

- `schan-recv-value [^borrow c] : T` -- the blocking read; leaves the channel.
- `schan-advance-recv [c] : (SChan R)` -- the protocol retag, the same pointer,
  exactly what `schan-send` already does at the end of its own body.
- `schan-recv` is then `(pair (schan-recv-value c) (schan-advance-recv c))`.

Constructing the Pair inside the inline-C body would mean hand-rolling the
struct layout -- a second copy of a definition the compiler owns, which is the
shape the cell out-parameter existed to avoid in the first place. Same split
`stdlib/env.tur` uses for `env/get` over `env/get-raw`, and the one that kept
`arc-upgrade` leak-free earlier on this branch.

`schan-cell-new` / `schan-cell-get` / `schan-cell-free` are **removed**. They
existed only to serve the workaround, and leaving them would leave a second
way to spell a receive.

## The interpreter needed the same migration

`src/turi/interpreter_natives.c` had native overrides for the cell-based
`schan-recv` and the three cell helpers. Without updating them the tree-walker
kept the old two-argument contract and `schan-roundtrip` failed under
`run-turi.sh` while passing compiled -- the two paths disagreeing, which is
worse than either being wrong.

The natives now override only the two inline-C **leaves**. `schan-recv` itself
is left to the tree-walker, so it builds the Pair with the same struct
machinery the compiled path uses. Overriding `schan-recv` instead would have
meant hand-building a Pair value in the interpreter -- the very copy this
change removes from the C side.

## Tests

Three fixtures migrated, none of them weakened:

- `tests/fixtures/schan-roundtrip` -- single round trip; runs on **both**
  harnesses and agrees.
- `tests/fixtures/schan-worker-pool` -- request/response across worker threads,
  including the non-owning-handle discards the linearity checker requires.
- `tests/fixtures/errors/schan-skip-step` -- the phantom-mismatch negative,
  whose `TUR-E0001` is unaffected by the signature change.

Suites: run.sh 2676 passed / 0 failed; run-turi.sh 1843 passed / 0 failed;
run-stdlib-checks.sh 35 / 0; run-flags.sh 86 / 0.

## Guide updated

docs/guides/session-types-guide.md -- the import list, the operation table,
and the round-trip example all use the pair form; the closing note that said
"the stdlib API simply has not been migrated yet" now records that it has, and
why the cell existed.

Regenerated `stdlib/docstrings.tur` and `docs/api/`.
