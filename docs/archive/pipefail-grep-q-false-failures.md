# `cmd | grep -q` under `set -o pipefail` reports false failures on large payloads

**RESOLVED 2026-08-21.** Swept: 0 real instances remain in any harness that
sets `pipefail`, and `tests/check-no-pipe-grep-q.sh` (ctest
`tur_no_pipe_grep_q`) now blocks reintroduction.

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


---

## Resolution

### The sweep

157 call sites across 40 files, rewritten to `grep -q PAT <<< "$var"`. The
here-string was chosen over the bash-glob form on purpose: `[[ == *glob* ]]` is
also immune, but the sites use `-qx`, `-qF`, `-qE`, `-qi` and `-qiE`, and a
blanket glob substitution would have silently changed line-anchored and regex
matches into substring matches. The here-string preserves grep's semantics
exactly -- every flag, every pattern -- so the sweep is a pure
delivery-mechanism change.

Verified immune on the same 400k-line payload that breaks the pipe form:

| form | false no-match / 10 |
|---|---|
| `printf ... \| grep -q` | **10** |
| `grep -q PAT <<< "$var"` | 0 |
| `[[ "$var" == *PAT* ]]` | 0 |

Six sites needed hand-work rather than the mechanical pass:

- Three `head -1 ...` / `ls "$dir" \|` command-LHS pipes, rewritten as
  `<<< "$(cmd)"`.
- Two `grep -qx build` with an **unquoted** pattern, which the quote-aware
  regex correctly declined to touch.
- One `run-repr-trace.sh:232` left alone on purpose: it is
  `|| grep -q PAT "$file"` -- a logical-or onto a *file* grep. No pipe, no
  writer, not an instance. The lint's matcher excludes `||` for this reason.

### One regression, caught by baselining

The first mechanical pass broke `tur_flags_tests` (86 -> 84). The tail-matcher
in my transform matched a `;` **inside** a quoted grep pattern --
`grep -q "; reader set to ..."` -- and split the line there, producing
`grep -q " <<< "$out"; reader set to ..."`. Three lines, all in
`run-flags.sh`, all repaired by hand.

This is the whole argument for capturing a baseline before a 157-site
mechanical sweep: a syntax check passed all three mangled lines, because they
*were* valid bash -- just grepping for a different string.

### Prevention: `tests/check-no-pipe-grep-q.sh`

Registered as ctest `tur_no_pipe_grep_q`. It rejects `... | grep -q` in any
`tests/**/*.sh` or `tools/**/*.sh` that sets `pipefail`, skipping comment-only
lines (so the immune spellings can be documented) and `||` (so a file grep is
not a false positive). Its header carries the mechanism, the three known
occurrences, the three immune spellings, and the instruction **not** to "fix"
an instance by dropping `pipefail`.

Verified in both directions: clean on the swept tree, and it catches a
deliberately reintroduced `echo "$x" | grep -q ...` (exit 1, naming file and
line).

### A note on what was already there

Four of the surviving matches were **comments other people had already written
warning about this exact pattern** -- `run-repr-trace.sh:175` and `:257`
("Grep files, never `... | grep -q`"), `run-flags.sh:834` ("match with bash
globs, not `echo "$out" | grep -q`"), and `repl-spice-watch.sh:175`. The
knowledge was in the tree; it just had no enforcement, so each new harness
reintroduced the bug. That is the gap the lint closes.

## Verification

- `ctest -E '^tur_tests$'` -- **108/108 before the sweep, 108/108 after**,
  then 109/109 with the new lint target. Same set, same results: the sweep is
  behaviour-neutral.
- `bash -n` on all 40 touched files
- `tests/run-flags.sh` -- 86 passed, 0 failed (matching its pre-sweep baseline)
