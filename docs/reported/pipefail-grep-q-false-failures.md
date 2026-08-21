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
