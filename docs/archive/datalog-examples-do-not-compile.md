# Four of the five examples/datalog/*.tur fail `tur check`

**PARTIALLY RESOLVED 2026-08-20; the codegen half closed 2026-08-21 (see the
follow-up below). The runtime residue moved to
`docs/reported/examples-tree-does-not-run.md`.** `tur check` now passes on 4 of 5 (was
1 of 5), and a ratcheted sweep stops any example rotting silently again.
But the report understated the damage in two ways -- see below. **None of
the five actually runs**, and the wider `examples/` tree is 9-of-17 broken.

**Severity: medium** -- the datalog tutorial series (datalog-01..05) walks
through code that does not compile, so anyone following it hits a wall.
Found 2026-08-20 while resolving `stdlib-cstr-content-equality`.

## Repro

```sh
for f in examples/datalog/*.tur; do
  printf '%-34s ' "$f"; ./build/tur check "$f" >/dev/null 2>&1 && echo OK || echo FAIL
done
```

| File | Result |
|---|---|
| `examples/datalog/blog.tur`    | FAIL |
| `examples/datalog/datalog.tur` | FAIL |
| `examples/datalog/indexed.tur` | OK |
| `examples/datalog/minimal.tur` | FAIL |
| `examples/datalog/query.tur`   | FAIL |

## Root cause

Two distinct defects.

**1. `pred` callback is an untyped/`:int`-erased parameter, then applied.**
`blog.tur:180`, `minimal.tur:145`, `query.tur:171`:

```
error: 'pred' is not a function or continuation
```

`db-q`'s signature (`examples/datalog/minimal.tur:138`) is
`(defn db-q [db : int pred] : ptr<void>)` -- `pred` carries no type at all, so
the elaborator has nothing to apply. This is the "No Lazy `:int` Stand-Ins"
defect from CLAUDE.md in its callback form: the parameter wants a real function
type, `(fn [Datum] bool)` or similar.

`indexed.tur` is the one that checks, which makes it the reference for what
the fixed shape should look like.

**2. `datalog.tur` does not parse at all.**

```
examples/datalog/datalog.tur:306:1: error: unterminated list (missing ')')
```

A structural paren defect, independent of the callback issue.

## Why this was not fixed in place

Nothing under `tests/` or `CMakeLists.txt` references `examples/datalog`, so
these files are not compiled by any suite -- which is how they drifted. Fixing
them means retyping `db-q`'s callback across four files and re-deriving the
truncated tail of `datalog.tur`, and the five tutorial guides
(`docs/guides/datalog-01..05`) quote this code, so the guides move with it.

## Fix direction

1. Give `db-q` (and its siblings) a real function type for `pred`, following
   whatever `indexed.tur` does that works.
2. Repair `datalog.tur`'s unterminated list.
3. Re-sync the quoted snippets in `docs/guides/datalog-0*.md`.
4. Add the examples to a suite so they cannot silently rot again -- a
   `tur check` sweep over `examples/` is enough and is cheap.

## Guides to update when fixed

- docs/guides/datalog-02-minimal-impl.md
- docs/guides/datalog-03-query-combinators.md
- docs/guides/datalog-04-indexing.md
- docs/guides/datalog-05-blog-example.md


---

## Resolution (partial -- read "What is still broken")

### Fixed: the `pred` callback type, and two paren defects

`db-q`'s `pred` and the whole combinator layer (`q-entity`, `q-attr`, `q-ea`,
`q-and`, `q-or`, `q-not`) now carry the real function type `(fn [int] bool)`,
across `minimal.tur`, `query.tur` and `blog.tur`. Verified the shape works as
a parameter type, a return type, and under composition before touching the
examples.

Two structural defects the report did not separate:

- `datalog.tur` was missing exactly one `)` in `match-pattern`.
- `blog.tur` had a stray `)` on line 293 that closed
  `(let [comment-entity ...])` immediately, putting its whole body outside the
  binding scope -- which is why `comment-entity` read as unbound.

`tur check`: **1 of 5 -> 4 of 5**.

### The report was wrong about `indexed.tur`

It called `indexed.tur` "the one that checks, which makes it the reference for
what the fixed shape should look like." It is not a reference for the callback
shape at all -- it has no `pred` and no `db-q`. It sidesteps predicates
entirely with an EAVT index and direct lookups, which is *why* it checked: it
never applies an untyped parameter. The right type had to be derived from
scratch.

### What is still broken: none of them RUN

`tur check` passing is not the bar, and the report treated it as one. On the
current tree:

| File | `tur check` | actually runs? |
|---|---|---|
| `blog.tur` | OK | **no** -- `cc` fails |
| `datalog.tur` | FAIL | -- |
| `indexed.tur` | OK | **no** -- segfault |
| `minimal.tur` | OK | **no** -- segfault |
| `query.tur` | OK | **no** -- `cc` fails |

Three distinct problems behind that:

1. **Segfault (`indexed`, `minimal`).** `indexed.tur` segfaults and **was
   never touched by this fix** -- it type-checked before and after. So the
   runtime breakage is pre-existing and wholly independent of the `pred`
   defect. It dies before its first `println`, i.e. in the hand-rolled
   `idb-new`/`idb-assert!` inline-C, not the query path. gdb puts it in `main`
   with no further symbols.

2. **`cc` failure (`blog`, `query`).** The generated C does not compile:

   ```
   examples_datalog_query_tur.c:7864: error: 'raw' undeclared
   examples_datalog_query_tur.c:7865: error: 'j' undeclared
   examples_datalog_query_tur.c:7870: error: 'key' undeclared
   ```

   These are **not** in the example's own code. They are inside
   `sch_hydecode_hyrec_hy`, i.e. `stdlib/schema.tur`'s inline-C, which arrives
   via the prelude (neither file has a `load` or `import`). Only one definition
   of that function is emitted, and the bad use sits between two `key`
   declarations that live in sibling scopes -- so this is a codegen scoping
   bug, not a stdlib source bug. `minimal.tur` emits the same function and
   compiles, so it is shape-dependent on the consuming program.

   **Attribution unverified.** It cannot be compared against `origin/main`
   directly, because `query.tur` does not type-check there -- the `pred` defect
   stops it before codegen. So this emission is only *newly reachable*. It may
   be a pre-existing codegen bug nothing previously reached, or an interaction
   with this branch's `:fn` adapter change. A reduced repro checked against
   main is the next step. Note it is the same *class* as one of the three
   defects in `httpd-mw-recover-unblocked-but-unwritten` ("an undeclared
   identifier").

3. **`datalog.tur` still fails check.** With the paren fixed, a real
   `TUR-E0201: cannot copy unique value 'e-val'` surfaces in
   `match-one-datum` -- `e-val` is consumed by `term-matches?` and then used
   again by `term-extend`.

Ruled out: this branch's new `stdlib/cstr.tur` `cstr-eq?` colliding with the
examples' local definitions. `stdlib/cstr.tur` is not autoloaded (verified --
a bare `cstr-eq?` call is an unknown-operator error), so there is no collision.

### The rot is wider than datalog

Sweeping the whole tree found **9 of 17** examples failing `tur check`, from
three unrelated causes: `cli_args_demo.tur` calls a `print` that does not
exist; `snake/src/main.tur` has an `import` outside `defmodule`; and the seven
`guestbook/src/*.tur` files need project context or stdlib loads they do not
perform (`store.tur`: "typeclass 'Serializable' is not defined").

### Fix-direction 4, delivered: `tests/check-examples.sh`

Registered as ctest `tur_examples_check`. It is a **ratchet**, not a gate --
`examples/examples-check-baseline.txt` lists today's known failures with a
reason each, and the sweep fails in **both** directions:

- a file not in the baseline starts failing (a regression), **and**
- a file in the baseline starts passing (drop it, so it becomes protected).

The second direction is what stops the baseline decaying into a list nobody
revisits. Both were tested by deliberately breaking each way, then restored.

## Follow-up 2026-08-21: remaining-work item 2 is done -- it was TWO codegen bugs

The `cc` failures in `blog.tur` and `query.tur` were not one bug and were not
in `stdlib/schema.tur`. The report guessed at "a codegen scoping bug ... inside
`sch_hydecode_hyrec_hy`"; the emitted C says otherwise. Each reduced to a
program of under a dozen lines with no datalog, no schema, and no httpd:

**`query.tur` -- an inline-C block cannot name a `let`-bound local.** A
function PARAMETER reaches inline C by its source name; a local took the
`<name>_<id>` path in `name_for_binding`, so `history`'s insertion sort --
`{ ... vec->data[j + 1] = key; }` over `raw`, `j`, `key` -- emitted three
`undeclared` errors. Reduced:

```turmeric
(defn f [x : int] : int
  (let [y (+ x 1)]
    (do ```c printf("%lld\n", (long long)y); ``` y)))
```

`x` compiled, `y` did not. Fixed by giving a local the same raw spelling as a
parameter when that is unambiguous -- a plain C identifier, unshadowed within
the function, not a keyword or libc symbol, and actually named by one of the
function's inline-C blocks (that last condition is what keeps codegen
byte-identical everywhere else). Pinned by
`tests/fixtures/inline-c-names-let-local/`, documented in
`docs/guides/c-integration-guide.md`.

**`blog.tur` -- a lifted lambda cannot read a top-level `def`.** Nothing to do
with the first bug. Pass 1 forward-declares every top-level FUNCTION, which is
why a lambda can call a `defn` defined later; top-level `def` STORAGE had no
such pass, and every lifted lambda is PREPENDED to the item list
(`elab_toplevel.c`, "Phase 3: Prepend file-scope definitions"), so it is
emitted above every `def` in the program regardless of source order. Reduced:

```turmeric
(def kw ":k")
(defn mk [] : int (apply1 (fn [d : int] : int (+ d (c2i kw))) 1))
```

`'kw_1334' undeclared`. Fixed by forward-declaring exactly the globals some
earlier-emitted item reads. Pinned by
`tests/fixtures/global-def-read-by-lifted-lambda/`.

The report's "attribution unverified" note can be closed: neither bug was an
interaction with that branch's `:fn` adapter change, and neither was newly
introduced -- both are structural, both predate the branch, and the `pred` fix
merely made them reachable. `run.sh` 2687/0 and `run-turi.sh` 1852/0 with
**zero snapshot churn**, which is the strongest evidence available that
nothing else depended on either spelling.

All five datalog examples now compile; `blog`, `indexed`, `minimal` and
`query` still segfault at runtime and `datalog.tur` still fails `tur check`.
That residue is now tracked as an OPEN report --
`docs/reported/examples-tree-does-not-run.md` -- rather than as a "Remaining
work" list inside an archived file, which is where it was invisible.

## Remaining work

*(Tracked in `docs/reported/examples-tree-does-not-run.md`; item 2 below is
resolved -- see the follow-up above.)*

1. Debug the two segfaults in the hand-rolled `rvec`/`db`/EAVT inline-C.
2. ~~Reduce the undeclared-identifier codegen bug and check it against
   `main`.~~ **Done 2026-08-21** -- two separate codegen bugs, both fixed.
3. Fix `datalog.tur`'s `TUR-E0201`.
4. Re-sync the quoted snippets in `docs/guides/datalog-0*.md` **after** the
   code actually runs -- syncing them now would just propagate broken code.
5. Work the other six baseline entries down.

## Verification

- `tur check`: 1 of 5 -> 4 of 5 datalog; whole tree 8 of 17 -> 8 passing plus
  9 documented as known-failing
- `ctest -R tur_examples_check` -- passes; ratchet verified in both directions
