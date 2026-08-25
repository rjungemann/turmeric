# `-main` is documented as the entry point but nothing calls it

**Severity:** medium. Two shipped examples build cleanly, link cleanly, run,
exit 0, and do nothing at all. A tutorial teaches the convention that causes it.

**Status:** RESOLVED 2026-08-25 via fix direction 1 -- the docs now match the
compiler. Both examples' entries are renamed `-main` -> `main`, and the snake
tutorial's 24 code listings and its entry-point claim are corrected. The
minikanren repro below now prints all four queries and exits 0.

Fix directions 2 (teach the compiler `-main`) and 3 (diagnose a `-main` with no
`main`) were NOT taken, and the report's "worth doing regardless" observation --
that no suite exercises the examples at all -- still stands. Both are filed as
[examples-have-no-suite-coverage](../reported/examples-have-no-suite-coverage.md).

**One claim in this report was wrong and is corrected here rather than edited
out.** The severity line says "two shipped examples build cleanly, link
cleanly, run, exit 0, and do nothing at all". That is true of
`examples/minikanren`, which is what the repro below exercises. It is **not**
true of `examples/snake`: that one does not compile at all, at `-main` or
`main` alike, failing on
[perform-inside-loop-has-no-lowering](../reported/perform-inside-loop-has-no-lowering.md)
-- a report that already names snake as the program it was found on. So snake
was never a silent no-op; it was a loud build failure that nothing ran often
enough to notice. The `-main` defect was real for both, but only minikanren
demonstrated it.

Found while censusing construction sites for
[SR0](../upcoming/sum-representation-plan.md): `examples/minikanren` reported
zero ADT constructions, which turned out not to be a property of the program.

## Repro

```sh
./build/tur emit-c examples/minikanren/src/main.tur > mk.c
cc -O2 -std=c99 -Lbuild/src mk.c -lturi -lm -lpthread -o mk
./mk ; echo "exit=$?"
```

```
exit=0
```

No output. The program's body -- four queries, each of which prints -- never
runs. That `cc` line is what `examples/minikanren/CMakeLists.txt` does, so
`just run-minikanren` (`./build/examples/minikanren/minikanren`) is a no-op too.

## Root cause

The example's entry point is spelled `-main`:

```turmeric
(defn -main [] : int
  (do
    (println "miniKanren-style relational example")
    ...))
```

It lowers to an ordinary function and nothing references it:

```c
static int64_t _hymain();      /* declared */
static int64_t _hymain() { ... }   /* defined -- and never called */
```

The emitted `main` runs static init and returns. There is no diagnostic: a file
with no `main` is not an error, and an uncalled `-main` is not a warning.

## Why this is a defect and not a misuse

`docs/guides/snake-game-tutorial.md:170` states it outright:

> - `-main` is the entry point

The tutorial's own listing declares `(defn -main [] ...)`, and its Step 1 test
instruction is "Run `./examples/snake/snake` -- you should see a black window."
Both shipped examples that follow the guide use `-main`:

| file | entry |
|---|---|
| `examples/minikanren/src/main.tur` | `-main` |
| `examples/snake/src/main.tur` | `-main` |

Nothing else in the tree uses it. Every fixture uses plain `main`, which is
why the suite never caught this -- the examples are not run by
`tests/run.sh`.

## Fix directions

Pick one; the first is the smallest.

1. **Make the docs match the compiler.** Rename both examples' entries to
   `main` and correct the tutorial line. Cheapest, and it is what every other
   program in the tree already does.
2. **Make the compiler match the docs.** Treat `-main` as an entry point when
   no `main` is defined. This is a real convention in some Lisps and is
   defensible, but it is new surface and needs a rule for what happens when
   both exist.
3. **Diagnose it either way.** A whole-program build that finds a `-main` and
   no `main` should say so. Even with fix 1 applied, the next person to follow
   the tutorial hits the same silent no-op.

Worth doing regardless of which: **the examples are not exercised by any
suite.** A program that builds, links, runs and prints nothing is
indistinguishable from a passing one when nobody checks the output.
