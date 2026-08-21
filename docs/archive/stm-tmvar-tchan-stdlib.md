# No TMVar/TChan in stdlib -- STM docs must hand-sketch them

**Severity: low** (minor expressiveness). Found in the 2026-08-20 docs audit.
**Status: RESOLVED** -- `stdlib/stm-sync.tur`.

## Repro

`grep -rn tmvar stdlib/` -> nothing (only interned-but-unused `sym_tmvar` /
`sym_tchan` in src/compiler/elab_core.c:2079). Both STM docs built the
patterns from `tvar/*` + `check` by hand.

## They had to be macros, and that is the interesting part

`atomically` requires a **syntactic** `stm` block. Verified against `main`:

```turmeric
(defn takeit [mv : ptr] : ptr (stm ...))
(atomically (takeit mv))
;; error: atomically requires an stm block as argument
```

So an STM action cannot be a value returned from a function -- it has to be
spliced into the transaction lexically, which is what a macro does. Every
operation here is therefore a macro, not because macros were preferred but
because a function literally cannot express one.

That constraint produced the two-form API. Each operation ships as:

- `<op>-stm` -- the transaction **body** only, to splice into your own
  `(stm ...)`. Several compose into ONE transaction that retries as a unit.
- `<op>` -- the same body wrapped in its own `(atomically (stm ...))`, for a
  standalone call.

The distinction is not cosmetic: two standalone `tmvar-take` calls are two
transactions with a window between them, while
`(atomically (stm (tmvar-take-stm a) (tmvar-take-stm b)))` drains both or
neither. The fixture asserts exactly that.

## Surface

**TMVar** (single-slot mailbox): `tmvar-new`, `tmvar-new-empty`,
`tmvar-take`, `tmvar-put`, `tmvar-read` (peek, leaves the value),
`tmvar-full?`, plus `-stm` variants of take/put/read.

**TChan** (unbounded FIFO): `tchan-new`, `tchan-write`, `tchan-read`,
`tchan-len`, `tchan-empty?`, plus `-stm` variants of write/read.

Two limitations stated in the module rather than glossed:

**A TMVar cannot hold 0.** An empty slot is the null pointer -- the same
representation the guides sketched, and in a pointer-valued world "no value"
genuinely *is* null rather than an `:int` sentinel standing in for a type. But
the consequence is real: 0 is indistinguishable from empty, so box the value
if you need the full range. A TChan has no such limit -- its empty state is
the empty *list*, so `(tchan-write ch 0)` round-trips, which the fixture
checks.

**TChan writes are O(n), reads O(1).** It appends at the tail of a cons list.
The right structure is a banker's queue (a front/back pair, reversing when
front empties, amortized O(1) both ways), which is not here because the
reverse-on-empty step has to run inside the transaction body and expressing
that in a macro that must also `check` and return a value is a lot of
machinery for a v1 primitive. Recorded in the module so a caller who hits the
write cost knows the fix rather than rediscovering the problem.

## Also fixed in passing

Explicitly `(load "stdlib/stm.tur")` -- which this module briefly did before
the constructors became macros -- surfaces two cc warnings in that file:
`tvar/new` returns a bare `void *` from a function whose `TVar` return type
lowers to `int64_t`, and `tvar/write` does `return 0;` from a `: nil` (C
`void`) function. Both are hygiene rather than miscompiles, both are two-line
fixes, and both are now fixed. They were invisible before because the tvar/*
functions normally arrive through the auto-load path.

## Tests

`tests/fixtures/stm-tmvar-tchan` -- the TMVar lifecycle (peek leaves the value,
take empties, put refills), TChan FIFO ordering (the assertion that separates
it from a stack), `tchan-len` between operations, the 0-as-a-value case, and
the two-takes-in-one-transaction composition that would be impossible with
functions.

Suites: run.sh 2676 passed / 0 failed; run-turi.sh 1843 passed / 0 failed;
check-cc-warn-ratchet OK.

## Guides updated

- docs/guides/stm-guide.md -- the "Building Higher-Level Primitives" TMVar
  section now names the module and its API, and says why the operations are
  macros. The sketch is kept: it shows the mechanism, which is that `check` is
  what turns a read into a blocking wait.
- docs/guides/stm-tutorial.md -- "There are no built-in TMVar/TChan types --
  both are small patterns you build" now points at the module first and keeps
  the sketches as explanation rather than instruction.

Regenerated `stdlib/docstrings.tur` and `docs/api/`.
