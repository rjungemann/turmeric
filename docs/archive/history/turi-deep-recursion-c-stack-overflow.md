# turi: deep non-tail recursion overflows the C stack (SIGSEGV) before the recursion-depth guard fires

> **RESOLVED (2026-06-12, Direction A).** `turi_env_new` now derives
> `max_eval_depth` from `getrlimit(RLIMIT_STACK)` divided by a conservative
> per-`eval_expr`-frame cost (`turi_default_max_eval_depth`, `src/turi/env.c`),
> targeting ~50% of the measured crash depth. Deep non-tail recursion now
> emits a clean `eval: recursion limit exceeded` (rc nonzero) in both
> Debug/ASan and Release -- never a SIGSEGV. This is the report's "smallest
> fix": it makes the guard reachable but caps achievable depth *below* the
> C-stack ceiling, so the parity gap is narrowed, not closed, and
> `escape-deep-capture` stays `requires.compiled`. The explicit-stack
> trampoline that removes the native-stack dependency (Direction D below) is
> planned in
> [`../../upcoming/v1/turi-eval-trampoline-plan.md`](../../upcoming/v1/turi-eval-trampoline-plan.md).
> Indexed in
> [`../../reported/resolved-paper-trail.md`](../../reported/resolved-paper-trail.md).

**Summary:** The tree-walking interpreter has a recursion-depth guard
(`eval_depth >= max_eval_depth -> "eval: recursion limit exceeded"`,
`src/turi/eval.c:3676`) whose default limit is **4096**
(`src/turi/env.c:83`). But a bounded, non-tail recursion blows the native C
stack at only **~650-700 frames** -- far below 4096 -- so the program dies with
a raw **SIGSEGV / stack-overflow** instead of the intended clean error. The
guard is effectively dead code: the C stack always overflows first.

**Severity:** Medium. Not a miscompile -- it is a robustness / parity gap. A
program that recurses a few hundred deep runs fine *compiled* but crashes hard
*under `--interpret`* (and in `tur repl`), and crashes **without any
diagnostic** in a Release build (`rc=139`, empty stderr). The recursion guard
that exists specifically to turn this into a graceful error never gets the
chance to run.

## Minimal repro

No continuations involved -- plain non-tail recursion (`/tmp/deeprec.tur`):

```turmeric
(defn sum-to [n : int] : int
  (if (= n 0)
    0
    (+ 1 (sum-to (- n 1)))))   ; (+ 1 ...) keeps it non-tail

(defn main [] : int
  (println (sum-to 5000))
  0)
```

### Observed

| Build | `sum-to 600` | `sum-to 700` | `sum-to 5000` |
| --- | --- | --- | --- |
| Release (`build-release/tur`) | `600` (rc 0) | **SIGSEGV** (rc 139, empty stderr) | SIGSEGV |
| Debug/ASan (`build/tur`) | `600` (rc 0) | SIGSEGV ~800 | `AddressSanitizer: stack-overflow ... in eval_expr_impl src/turi/eval.c:3684` |

(Stack limit on the box: `ulimit -s` = 12500 KB.)  The crash threshold
(~650-700 frames) is nearly identical across Release and ASan, so this is a
genuine per-frame C-stack cost, not an ASan-redzone artifact.

### Expected

At some bounded depth the interpreter emits **`eval: recursion limit
exceeded`** (rc nonzero, message on stderr) -- the behavior `max_eval_depth`
was added to provide -- rather than a SIGSEGV. The compiled path runs
`sum-to 5000` fine; the interpreter should at worst fail gracefully.

## Root cause

Two interacting facts:

1. **The guard limit is set far above what the C stack can hold.**
   `turi_env_new` sets `env->max_eval_depth = 4096` (`src/turi/env.c:83`); the
   guard at `src/turi/eval.c:3676-3677` only fires at `eval_depth >= 4096`.
   Reaching depth 4096 would require ~4096 nested C frames.

2. **Each logical recursion level costs several KB of C stack.** One
   `(sum-to ...)` level walks through, on the C stack:
   `eval_expr` (the depth-guard wrapper, `eval.c:3668`) ->
   `eval_expr_impl` (`eval.c:3684`) -> `eval_apply` -> `eval_body_tco`
   (`eval.c:3322`) -> `eval_expr` -> ...  `eval_expr_impl` is a single giant
   `switch` whose frame carries multiple `TuriValue args[MAX_EVAL_ARGS]` /
   `fields[MAX_EVAL_ARGS]` stack arrays with `MAX_EVAL_ARGS == 64`
   (`eval.c:3662`, used at `:3849`, `:3949`, `:3993`, ...).  With
   `sizeof(TuriValue)` ~16-24 B, each such array is ~1-1.5 KB and the function
   reserves several, so a single level burns on the order of ~15-18 KB of
   stack across the wrapper + impl + apply + tco chain.  At ~12 MB of stack
   that exhausts around ~650-700 levels -- i.e. the guard's 4096 is ~6x too
   high to ever be reached.

So `eval_depth`/`max_eval_depth` increments once per `eval_expr` call
(`eval.c:3678`) and tracks the C-stack nesting faithfully, but the *limit* was
chosen without reference to the actual per-frame stack cost, leaving the guard
unreachable in practice.

## Discovery

Surfaced by `tests/fixtures/escape-deep-capture` while landing call/cc /
escape in the interpreter
([turi-capturing-shift-unimplemented.md](../archive/turi-capturing-shift-unimplemented.md)):
that fixture invokes an escape continuation from 5000 non-tail frames to prove
capture is O(1) at any depth. The escape *semantics* work under `--interpret`
(verified at depths 50/500; see `continuation-escape`, `escape-real`,
`escape-nested-reset`), but the 5000-frame **build-up** crashes first. The
fixture is currently marked `requires.compiled` with this defect as the reason;
it can be returned to the `--interpret` allowlist if/when the achievable depth
grows past 5000.

## Proposed fix directions

- **(A) Make the guard reachable (smallest fix).** Set `max_eval_depth` from
  the actual stack limit rather than a fixed 4096: read `getrlimit(RLIMIT_STACK)`
  at `turi_env_new` and divide by a measured/estimated per-frame cost (with a
  safety margin), or simply lower the default to a value the stack provably
  sustains (empirically ~500 here). Trade-off: caps legitimate interpreter
  recursion lower, but a clean `recursion limit exceeded` beats a SIGSEGV.
- **(B) Shrink the per-frame cost (raises the ceiling).** The
  `TuriValue args[MAX_EVAL_ARGS]` / `fields[MAX_EVAL_ARGS]` arrays in
  `eval_expr_impl` dominate the frame. Move the rarely-large cases
  (`EX_CALL`, `EX_BUILTIN`, `EX_MAKE_STRUCT`, ...) into separate
  non-inlined helper functions so the hot `eval_expr_impl` frame stays small,
  and/or heap-allocate the arg scratch for high-arity calls. This multiplies
  the achievable depth for the same stack.
- **(C) Combine A+B**: shrink frames *and* derive `max_eval_depth` from the
  stack limit, so the guard fires just below the (now higher) real ceiling.
- **(D) Trampoline / explicit-stack evaluator** (large rework) -- removes the
  native-stack dependency entirely. Out of scope for a point fix; note for a
  future interpreter-performance pass.

## How to validate a fix

```sh
# Should print a clean error, NOT SIGSEGV (rc 139):
./build/tur --interpret /tmp/deeprec.tur    # sum-to 5000
echo $?    # expect nonzero, with "recursion limit exceeded" on stderr
```

A fix is good when (1) deep non-tail recursion yields `eval: recursion limit
exceeded` (or runs to completion), never a SIGSEGV, in **both** Debug/ASan and
Release builds; and (2) the guard's effective limit sits comfortably below the
empirical crash threshold. If fix (B)/(D) raises the ceiling past 5000, drop
the `requires.compiled` marker on `tests/fixtures/escape-deep-capture` and add
it to the `run-turi.sh` allowlist.

## Status

Filed while completing the call/cc / escape interpreter work. Pre-existing
defect (independent of continuations); the guard at `eval.c:3676` has been
ineffective since `max_eval_depth = 4096` was set. Tracked here; not yet fixed.
