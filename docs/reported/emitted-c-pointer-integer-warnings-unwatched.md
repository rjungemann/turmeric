# Nothing watches the emitted C for pointer/integer confusion warnings

**Severity:** low -- a prevention gap, not a live defect. **The sweep this report
was opened to propose has been run, and the corpus is clean** (0 hits across
2563 fixtures). What is missing is anything that keeps it that way: `cc`
warnings on the emitted C are discarded on a successful build, so the class can
reappear silently. It is filed because the class is demonstrably reachable and
the ratchet is nearly free -- the suite already captures the output it would
need to check.

## Why this class matters

`-Wint-conversion` and `-Wincompatible-pointer-types` are the C compiler saying
a pointer and an integer were confused. In a language whose ABI deliberately
carries handles as `int64_t`, that is exactly the boundary where a
representation bug shows up -- and it shows up as a **warning**, which the
suite ignores, rather than an error, which fails a fixture.

It is not hypothetical. Until 2026-08-06, this compiled:

```turmeric
(defn carry [c : (Cons int)] : int c)
```

emitting `return (int64_t)(intptr_t)c;` in a function whose C return type is
`tur_adt_Cons__int *`:

```
warning: returning 'long int' from a function with return type
  'tur_adt_Cons__int *' makes pointer from integer without a cast
  [-Wint-conversion]
```

Under `-Werror` that is a hard failure. It was found by reading `cc` output by
hand while executing an unrelated report
([struct-return-type-mismatch-unchecked-until-cc](../archive/struct-return-type-mismatch-unchecked-until-cc.md)),
not by any check -- and the return position now rejects it at the source, which
is why the corpus is clean today.

## The sweep -- method and result

Every fixture directory with an input (2563 of them -- the same set `run.sh`
compiles), built the way the suite builds them:

```sh
export TUR_CC_FLAGS="-O2 -std=c99 -Wall -fno-strict-aliasing -L$(pwd)/build/src"
./build/tur build "$input" -o "$tmp" 2>&1 \
  | grep -E '\[-Wint-conversion\]|\[-Wincompatible-pointer-types\]'
```

**Result: 0 hits.** Wall clock ~4m20s across `nproc` workers.

Harness validated with a canary that still type-checks, so the zero is a real
measurement and not a broken pipeline:

```turmeric
(defn f [] : cstr
  ```c
  return 42;
  ```)
```

-> `warning: returning 'int' from a function with return type 'const char *'
makes pointer from integer without a cast [-Wint-conversion]`, caught by the
grep above.

### A trap worth recording

A first pass tried the cheap route -- `tur emit-c` per fixture, then
`cc -fsyntax-only` on the standalone `.c`. It reported **32 hits**, all in
`httpd-*` fixtures, all of the shape *"returning 'int' from a function with
return type 'const char *'"*. Every one was an artifact: the emitted file does
not compile standalone (`error: unknown type name 'HttpdConn'` in the same
run), so a callee like `httpd_conn_own_cstr` was implicitly declared as
returning `int` and the "conversion" was the missing declaration, not the
codegen. A real `tur build` of the same fixture emits **zero** warnings of any
kind.

Two lessons for anyone re-running this: build the fixture the way the suite
does rather than syntax-checking the emitted C in isolation, and do not pass
`-w` before `-Wint-conversion` -- GCC's `-w` wins over the later `-W` flag, so
an entire sweep silently reports nothing (that was the pass before this one).

## Fix direction

Fail, or at minimum report, on these warning classes in `tests/run.sh`. The
expensive part already runs: line 607 captures the build's stderr to
`$out_dir/actual.stderr` and currently reads it only when the build FAILS. On
success it is discarded. Grepping the same file for
`\[-Wint-conversion\]|\[-Wincompatible-pointer-types\]` after a successful
build costs one `grep` per fixture and needs no second compile.

Open questions for whoever implements it:

- **FAIL or WARN?** The corpus is at zero, so FAIL is affordable today and is
  the only setting that keeps it at zero. A `WARN`-plus-count line is the
  softer option if a platform turns out to warn where Linux/GCC 13 does not.
- **Which warnings?** Start with the two pointer/integer classes; they are the
  ones that mean "a representation crossed wrong". A broader `-Wall` ratchet is
  a different, larger project -- do not conflate them.
- **Per-platform variance.** This sweep was GCC 13.3 on Linux. Clang words
  these differently and macOS/Windows legs may differ; match on the bracketed
  warning name where possible, and expect to widen the pattern.

## Notes

This is deliberately **not** filed as a value-representation open cell. No
representation is wrong today and there is no failing input; the guide's
open-cells table tracks live cells, and a clean-corpus prevention gap is not
one. If the ratchet lands and immediately turns something red, that finding
gets its own row there.

## Found while

Executing
[struct-return-type-mismatch-unchecked-until-cc](../archive/struct-return-type-mismatch-unchecked-until-cc.md),
whose fix rejected the `(Cons int)` case above at the source. The observation
that other such sites might exist was recorded there as a loose end; this report
is that loose end followed to its conclusion.
