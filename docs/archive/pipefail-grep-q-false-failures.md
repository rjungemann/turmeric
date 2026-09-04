# `cmd | grep -q` under `set -o pipefail` reports false failures on large payloads

**Severity: medium** (flaky tests that read as product bugs). Found 2026-08-20
when two targets failed CI on a branch that could not have affected either.
This is the **third** occurrence in this repo.

## Mechanism

`grep -q` exits the moment it finds its first match. If the writer on the left
of the pipe is still producing output, it takes SIGPIPE and dies with 141.
Under `set -o pipefail` the pipeline's status is that 141 -- a **failure** --
which means:

> the pipeline reports failure precisely **because** the pattern was found.

An `if ! ... | grep -q PATTERN` therefore takes the "not found" branch when the
pattern IS there. Whether it fires depends on whether the writer finishes
before grep exits, so it scales with payload size and machine load: green by
hand, red in CI, and green again on the next re-run.

## Repro

```bash
set -euo pipefail
big=$(python3 -c "
print('MATCHME')
for i in range(400000): print('filler line %d' % i)
")
for i in $(seq 10); do
  if ! printf '%s\n' "$big" | grep -q "MATCHME"; then echo "false no-match"; fi
done
```

Prints `false no-match` **10/10**, with the pattern sitting on line 1.

## Occurrences

| When | Site | Symptom |
|---|---|---|
| earlier | `rp6-watch-with-help` | fixed in 5600eff2 ("SIGPIPE/pipefail false failure") |
| 2026-08-20 | `tests/check-export-from-no-wrapper.sh:47` | `call site does not dispatch to chain__low__low_hyadd` -- pipes a whole emitted C file |
| 2026-08-20 | `tests/run-build-project.sh:967` | `build-project-manifest-fx-row-rejected-cmake-deps -- rc=1 out=<...TUR-E0620...>` -- the failure message contains the very text the assertion greps for |

The second one is the tell that this is not a product bug: the harness printed
`rc=1` and a diagnostic containing `TUR-E0620`, which is exactly the pair of
conditions the assertion requires to PASS.

Both are fixed (bash substring match, no pipe). The class is not swept.

## Remaining exposure

`grep -rn '| *grep -q' tests/ --include=*.sh` over files that `set -o pipefail`
finds roughly 40 more call sites. Most pipe a handful of lines of REPL or
program output, where the writer wins the race in practice -- but the property
is machine- and load-dependent, not structural, so "it passes today" is not a
guarantee. The ones piping large payloads are the live risk;
`tests/run-build-shared.sh:63` pipes `nm` output and is the most obvious next
candidate.

## Fix direction

Sweep the remaining sites. Any of these is immune, in rough order of
preference:

1. `[[ "$var" == *"needle"* ]]` -- pure bash, no subprocess, no pipe.
2. `grep -q needle <<< "$var"` -- here-string, nothing to SIGPIPE.
3. `printf '%s' "$var" | grep -c needle > /dev/null` -- `-c` reads to EOF, so
   no early exit (but still spawns a process).

Do **not** "fix" it by dropping `pipefail`; that hides real failures in the
writer.

A lint would be cheap insurance: reject `| grep -q` in any `tests/*.sh` that
sets `pipefail`, pointing at this note.

## Resolution (2026-08-21)

Swept and gated. Three parts:

1. **Sweep.** Every `WRITER | grep -q ...` in a `tests/*.sh` that sets
   `pipefail` is rewritten to a here-string (`grep -q PATTERN <<< "$var"`) --
   180 call sites across 39 scripts. One site whose writer is a command
   (`head -1 script.zsh`) uses process substitution
   (`grep -q PATTERN < <(cmd)`) instead: the writer is then not part of the
   pipeline, so its SIGPIPE cannot become the status either.
2. **Lint.** `tests/check-pipefail-grep-q.sh` fails on any pipe-into-`grep -q`
   in a script that sets `pipefail`, and is registered as the
   `tur_pipefail_grep_q_lint` ctest target. It carries the mechanism and the
   three immune spellings in its header.
3. **Four sites the sweep's own regex missed**, found *by* the lint on its
   first run: `grep -E -q` / `grep -F -q` spell the flags as two tokens
   (`tests/run-jit.sh:370,384`, `tests/run-leak-gate.sh:60,80`). The lint
   matches any flag ordering, so these are covered going forward.

Two harness defects surfaced while verifying the sweep, both fixed here and
neither caused by it (each reproduces identically on the pre-sweep files):

- `tests/turi/repl-spice-load.sh` built `TUR_BIN="$PWD/$TUR"` unconditionally,
  so an **absolute** `$TUR` became `/home/user/turmeric//home/user/turmeric/build/tur`
  -- which still starts with `/`, so the absolutise-if-relative guard right
  below it passed the corrupted path through. 6 of its 9 assertions failed
  whenever the caller exported an absolute `TUR`.
- `tests/turi/eval-async-io.sh` did not opt out of leak detection the way its
  siblings do. `read-async` hands its result to `turi_cstr`, which *borrows*
  the pointer (the interpreter never frees, by design), so LSan makes
  `tur repl` exit 1 and the script died under `set -e` before its first
  assertion -- no output, exit 1. It passed under `run-turi.sh` only because
  that harness exports `detect_leaks=0` suite-wide.

Verified: all 39 swept scripts pass `bash -n`, and every one that runs on this
box was executed green (run-flags 86/86, run-build-project 38/38, the four
hamt harnesses, run-build-shared 11/11, run-dap 20/20, run-debugger 17/17,
the leak gates, and the turi repl/eval set).
