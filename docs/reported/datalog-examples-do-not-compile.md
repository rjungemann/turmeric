# Four of the five examples/datalog/*.tur fail `tur check`

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
