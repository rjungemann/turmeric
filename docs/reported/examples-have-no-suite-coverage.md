# No suite exercises `examples/`, and nothing diagnoses a program with no entry point

**Severity:** medium. This is the residue of
[dash-main-entry-point-never-invoked](../archive/dash-main-entry-point-never-invoked.md),
whose headline defect is fixed. That bug survived in two shipped examples for
as long as it did because of the two gaps below, and neither is closed.

**Status:** OPEN. Not fixed -- filed while resolving the `-main` report.

## 1. `examples/` is not exercised by any suite

`tests/run.sh` walks `tests/fixtures/`. Nothing walks `examples/`. The examples
build via their own CMakeLists, and `just run-minikanren` runs one by hand, but
no automated check ever compares an example's output against anything.

That is why `-main` went unnoticed: `examples/minikanren` and `examples/snake`
built cleanly, linked cleanly, ran, and exited 0 while printing **nothing at
all**, and a program that prints nothing is indistinguishable from a passing one
when nobody checks the output.

The gap is wider than the one bug. An example is the most-read code in the
project -- it is what a new user copies -- and it is the only code with no
regression net under it.

**Fix direction.** The cheap version is a harness in the shape of
`tests/run.sh`: for each `examples/<name>/` carrying an `expected.stdout`,
build it and diff. `minikanren` is a pure computation and would work today.
`snake` opens a window, so it needs either a headless mode or a
`requires.display`-style skip marker -- the existing `requires.*` convention
already covers exactly this case.

**A build-only check would already pay for itself**, before any output diffing:
`examples/snake` does not currently compile. It fails on
[perform-inside-loop-has-no-lowering](perform-inside-loop-has-no-lowering.md),
which has been open since 2026-08-21 and names snake as the program it was
found on. A shipped example has been un-buildable for days without anything
saying so, which is the same gap as the silent no-op wearing a different
face.

## 2. A whole-program build with no entry point says nothing

Independent of the examples, and the reason fix direction 1 alone is not
enough: a program that defines no `main` is not an error, and a function that
looks like an entry point but is not called produces no diagnostic. The emitted
`main` runs static init and returns 0.

The next person to follow a tutorial, port code from a Lisp that uses `-main`,
or typo the name gets the same silent no-op. Nothing in the compiler is in a
position to notice -- but something could be:

- Direction 3 of the original report: a whole-program build that finds a
  top-level `-main` (or `main-`, or any near-miss) and no `main` should say so.
  A warning is enough; it does not need to be an error.
- The more general version: a whole-program build that synthesizes an empty
  `main` -- no user `main`, no top-level statements to fold into one -- is
  almost certainly not what the author meant, whatever the near-miss name is.

The second is the better rule because it does not depend on guessing which
misspelling to look for, and it catches the case where the entry point is named
something else entirely.

**Not done here** because it is new diagnostic surface (a code assignment, a
fixture, a docs entry) rather than the doc correction the `-main` report was
resolved with.
