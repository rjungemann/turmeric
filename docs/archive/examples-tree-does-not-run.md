# `examples/` compiles but does not RUN -- four segfaults and nine `tur check` failures

> **RESOLVED 2026-08-21, same day it was filed.** Everything in `examples/`
> that compiles now also runs, and a run-ratchet defends that.
>
> - **Item 1 -- the four segfaults.** Example code, not the compiler, and the
>   ASan report named it in seconds exactly as this report predicted:
>   `rvec-get`'s inline C did `return (int)vec->data[i];`. C's `int` is 32
>   bits, so a 64-bit datum pointer came back truncated and the next
>   `datum-value` dereferenced `0x70`. All five datalog files carried the same
>   line (and a matching `(int)vec->len`). Dropping the cast fixed all four.
> - **Item 2 -- `datalog.tur`'s TUR-E0201.** Also example code. A `defdata`
>   moves by default, and `match-one-datum` legitimately uses `e-val` twice
>   (unify, then bind). `Value` is a tag plus a machine word with nothing to
>   own, so it wants `:copy`. Documented in
>   `docs/guides/datalog-02-minimal-impl.md`, which is where a reader meets the
>   trap.
> - **Item 3 -- the nine `tur check` failures.** Down to eight, and one of
>   those is not example code either. `cli_args_demo.tur` was fictional twice
>   over -- it called a `print` that does not exist AND a bare `getenv` that
>   does not exist -- and is rewritten against `env/get` + `str-concat`.
>   `snake/src/main.tur`'s `import`-outside-`defmodule` was a real one-line fix
>   (`load`), and behind it sits a compiler limitation now filed as
>   [perform-inside-loop-has-no-lowering](perform-inside-loop-has-no-lowering.md).
>   The guestbook seven are the invocation question this report raised, now
>   filed separately as
>   [guestbook-example-has-no-import-graph](guestbook-example-has-no-import-graph.md).
> - **Item 4 -- the run-ratchet.** Delivered. `tests/check-examples.sh` now
>   RUNS every example that checks clean and requires exit 0, against
>   `examples/examples-run-baseline.txt` (empty on purpose, and the sweep fails
>   if a listed entry starts running, so it cannot decay). Verified in both
>   directions; direction B was reproduced by restoring the `(int)` cast, which
>   the sweep reports as `checks clean but exited 139`.
>
> One more thing fell out, unrelated to the examples but found by running the
> sweep under the Debug (sanitized) `tur`: `(perform ...)` in statement
> position shares its `emit_stmt` case with `EX_HANDLE` and read
> `is_unsafe_marker` out of the wrong union member. A non-zero garbage byte
> there sends the emitter down the pure-`Unsafe` path and makes it emit a
> pointer read from the wrong union member. Fixed with a `kind` test, and
> because the trigger is uninitialized memory and cannot be pinned by a
> fixture, the sweep now also fails on any sanitizer line the compiler prints
> while checking an example -- which reproduces it on `snake` when the fix is
> reverted.

**Severity: medium.** Split out of
`docs/archive/datalog-examples-do-not-compile.md` on 2026-08-21, when the two
codegen bugs behind its `cc` failures were fixed. That report is archived and
its "Remaining work" list was only reachable from inside the archive, so the
residue was invisible to a triage pass. This file is that residue, tracked
where an open finding belongs.

## Repro

```sh
for f in examples/datalog/*.tur; do
  printf '%-34s ' "$f"; ./build/tur run "$f" >/dev/null 2>&1; echo "rc=$?"
done
```

| File | `tur check` | `tur run` |
|---|---|---|
| `examples/datalog/blog.tur`    | OK   | **139** (SIGSEGV) |
| `examples/datalog/datalog.tur` | FAIL | -- |
| `examples/datalog/indexed.tur` | OK   | **139** (SIGSEGV) |
| `examples/datalog/minimal.tur` | OK   | **139** (SIGSEGV) |
| `examples/datalog/query.tur`   | OK   | **139** (SIGSEGV) |

`tests/check-examples.sh` (ctest `tur_examples_check`) ratchets `tur check`
only. Nothing runs any example, which is how "compiles" came to be mistaken
for "works" twice in this file's history.

## Three separate items

### 1. Four segfaults in the hand-rolled inline-C data structures

`indexed.tur` type-checked before and after every fix applied so far, so its
segfault is **pre-existing and independent** of both the `pred`-callback defect
and the two codegen bugs. It dies before its first `println`, i.e. in the
hand-rolled `idb-new` / `idb-assert!` inline C, not in the query path; gdb puts
it in `main` with no further symbols. `blog`, `minimal` and `query` share the
same `rvec` / `db` layer.

The prime suspect is that layer's raw-pointer bookkeeping:

```turmeric
{ struct { int64_t *data; size_t len; size_t cap; } *vec = (void*)raw; ... }
```

is written out by hand in several places, and nothing checks that it matches
what `rvec-new` actually allocated. Note this is example code being wrong about
its own C, not necessarily a compiler defect -- **establish which before
filing a compiler bug**. Build one of them with
`TUR_DEBUG_SANITIZE` on and read the ASan report first; that is a five-minute
answer and it decides where the rest of the work goes.

### 2. `datalog.tur` -- `TUR-E0201: cannot copy unique value 'e-val'`

In `match-one-datum`: `e-val` is consumed by `term-matches?` and then used
again by `term-extend`. A real uniqueness error in the example, not a compiler
defect. Either clone before the first use or restructure so the second use
reads the result of the first.

### 3. Nine `tur check` failures across the wider tree

`examples/examples-check-baseline.txt` carries them with a reason each. Three
unrelated causes:

- `cli_args_demo.tur` calls a `print` that does not exist (only `println`).
- `snake/src/main.tur` has an `import` outside `defmodule`.
- the seven `guestbook/src/*.tur` need project context or stdlib loads they do
  not perform (`store.tur`: "typeclass 'Serializable' is not defined").

The guestbook seven are one fix, not seven -- they are a `build.tur` project,
and checking a member file standalone is arguably the wrong invocation. Decide
whether the sweep should check that project through its manifest before
"fixing" the files.

## Fix direction

1. ASan one segfaulting example; that answers whether items 1 is example code
   or compiler.
2. Fix the `TUR-E0201` in `datalog.tur` and drop its baseline row.
3. Work the other baseline entries down, starting with the two one-liners.
4. Extend `tests/check-examples.sh` to RUN the examples that have deterministic
   output, not just check them -- otherwise this rots again in exactly the same
   way. A run-ratchet needs its own baseline (the guestbook/snake ones want a
   display, a port, or a project context), so it is a second list, not a
   widening of the first.

## Guides to update when fixed

- docs/guides/datalog-02-minimal-impl.md
- docs/guides/datalog-03-query-combinators.md
- docs/guides/datalog-04-indexing.md
- docs/guides/datalog-05-blog-example.md

The five datalog guides quote this code. Re-sync them **after** it runs --
syncing now would just propagate code that segfaults.
