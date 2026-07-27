# `tur fmt` splits a fitting `defn` header when it carries a type-parameter list

**Severity:** low (cosmetic, and stable -- but it keeps `fmt-bootstrap-stdlib`
permanently red, which trains people to ignore that suite).

Surfaced as the long-standing `tests/run-fmt.sh` failure
`fmt-bootstrap-stdlib: stdlib is not self-formatted: stdlib/rcvec.tur`.
Independently confirmed as pre-existing by the agent working on
[Trowel](https://github.com/rjungemann/trowel) (17 passed / 1 failed both
before and after the `claude/busy-clarke-6zj9jl` formatter refactor).

## Summary

For a `defn` with a **type-parameter list** (`[A]`) followed by the ordinary
parameter vector, `tur fmt` breaks the header across three lines even when the
whole thing fits well inside the 80-column budget.

## Repro

```sh
$ ./build/tur fmt --stdout stdlib/rcvec.tur | diff stdlib/rcvec.tur -
```

```
119c119,121
< (defn rcvec-push! [A] [^borrow v : rc<RcVec> ^borrow item : rc<A>] : void
---
> (defn rcvec-push! [A]
>   [^borrow v : rc<RcVec> ^borrow item : rc<A>]
>   : void
```

The source line is **73 columns**; the second case (`rcvec-get`, line 154) is
**59**. Both are comfortably under `opts.line_width = 80`, so the split is not
a wrapping decision -- something in the layout path is treating the
type-parameter list as forcing a break.

Only these two forms in the file differ; the total diff is 12 lines.

## Not an idempotence bug

Worth separating, because the two failures look alike from the summary line:

```sh
$ ./build/tur fmt --stdout stdlib/rcvec.tur | ./build/tur fmt --stdin \
    | diff - <(./build/tur fmt --stdout stdlib/rcvec.tur)   # identical
```

`fmt(fmt(x)) == fmt(x)` holds. The formatter is self-consistent; it simply
disagrees with the checked-in source. So `fmt-idempotence-stdlib` passes and
only `fmt-bootstrap-stdlib` fails.

## Fix directions

Two ways out, and they are not equivalent:

1. **Fix the formatter** -- find why a `defn` with a type-parameter list skips
   the fits-on-one-line fast path in `src/compiler/fmt.c` and make it measure
   the whole header. This is the right answer if the one-line form is the house
   style, which the checked-in stdlib implies it is.
2. **Reformat `stdlib/rcvec.tur`** to whatever the formatter emits, making the
   suite green immediately. Cheap, but it bakes in a layout nobody chose and
   silently rewrites two readable lines into six.

Prefer (1). Reach for (2) only to unblock, and file the layout question rather
than letting the reformat stand as the decision.

Note that either fix is a fixture-snapshot no-op -- this is `tur fmt` output,
not codegen, so `tests/fixtures/*/expected.c` is unaffected.

## Related

`tests/run-fmt.sh` additionally fails to run its idempotence check at all on
macOS/BSD -- see
[fmt-idempotence-head-z-silently-skips-on-bsd.md](fmt-idempotence-head-z-silently-skips-on-bsd.md).
