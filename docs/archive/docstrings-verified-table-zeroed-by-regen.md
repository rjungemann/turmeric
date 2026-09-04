# `just docs` silently empties the `doc-verified?` table

> **RESOLVED 2026-08-20.** `gendocs.py` no longer treats "no doctest data" as
> "nothing is verified", and `run-doctests.sh` now says whether its manifest is
> complete. Fix directions (1) and (3) landed; (2) was declined on purpose and
> (4) is still open. Details in "Resolution" at the end.


**Summary.** `tools/gendocs.py` reads the `doc-verified?` name set from a
**gitignored build artifact** (`tests/doctest-generated/verified.txt`) and
writes it into a **tracked source file** (`stdlib/docstrings.tur`). When the
artifact is absent -- which is every fresh clone, every CI checkout, and any
worktree where `just doctest` has not been run -- absence is treated as "the
empty set" rather than "unknown", so the regen deletes the whole table and
`(doc-verified? name)` returns false for every name in the stdlib.

**Severity: low-medium.** Nothing crashes and no in-tree code calls
`doc-verified?` (its only mentions are its own definition and `(export
doc-lookup doc-verified?)`), so today this is a silently wrong answer from an
exported stdlib predicate rather than a broken build. It earns a report anyway
because of its shape: the loss is silent, it lands in a *tracked* file, and it
lands there specifically on the release path.

## Minimal repro

From a clean checkout (no `tests/doctest-generated/`):

```sh
$ awk '/static const char \*verified/,/NULL/' stdlib/docstrings.tur | grep -c '^    "'
142

$ python3 tools/gendocs.py stdlib/ --out /tmp/probe --emit-tur /tmp/docstrings.tur
  Wrote /tmp/docstrings.tur (1984 entries, 0 verified)

$ awk '/static const char \*verified/,/NULL/' /tmp/docstrings.tur | grep -c '^    "'
0
```

Create the manifest and it comes back proportionally:

```sh
$ mkdir -p tests/doctest-generated && printf 'atan2\nceil\n' > tests/doctest-generated/verified.txt
$ python3 tools/gendocs.py stdlib/ --out /tmp/probe2 --emit-tur /tmp/d2.tur   # -> 2 verified
```

Against `stdlib/docstrings.tur` itself (what `just docs` actually does) the
result is `142 deletions, 0 insertions`.

## Root cause

- `tools/gendocs.py:1764-1772` -- `verified_path.exists()` is the only guard.
  A missing file leaves `verified_names = None`, and
  `emit_docstrings_tur`'s docstring (`tools/gendocs.py:1508-1510`) makes the
  conflation explicit: *"When absent (or empty), `doc-verified?` always returns
  false."* Absent and empty are different facts -- "I have no doctest results"
  is not "no function passes its doctests" -- and only one of them should be
  allowed to overwrite a tracked file.
- `.gitignore:60` ignores `tests/doctest-generated/`, so the input is never in
  a fresh tree.
- `Justfile:322` (`docs:`) does not depend on `Justfile:61-63` (`doctest:`),
  which is what writes the manifest. Nothing in the default ordering produces
  the input before the consumer runs.
- `tools/run-doctests.sh:63` truncates `verified.txt` at the top of every run,
  so an interrupted or failed doctest run leaves a legitimately empty manifest
  that is indistinguishable, at the gendocs end, from a complete one.
- No CI job diffs the regenerated file against the committed one, so drift in
  either direction is invisible.

The reporting is not silent, exactly -- gendocs prints `(1984 entries, 0
verified)` (`tools/gendocs.py:1602`). But it is an info line in the middle of a
146-module pipeline, not a warning, and `just docs` is itself a mid-pipeline
step of `just wasm` -> `just deploy-web`.

## Why it matters more than "no consumer" suggests

The committed value of this table has never tracked which doctests pass. It
tracks **whether whoever last committed the file happened to have run `just
doctest` in that worktree.** Of the 116 commits that touch
`stdlib/docstrings.tur`, only 8 ever carried a non-empty table:

```
cf15a4281  142  2026-08-20
265580abf  115  2026-08-20
1db892cad  115  2026-08-20      <- seven commits on one day
4c512f345  115  2026-08-20
cafa773b9  115  2026-08-20
41d93fd39  115  2026-08-20
0e2cac699  115  2026-08-20
63ab24af2    1  2026-05-25
```

Every other commit -- including `4422074a3` ("Deploy stuff: regenerated wasm +
docstrings for v0.3x"), a release commit that regenerated this exact file --
shipped `verified = {}`. So `doc-verified?` answered false for everything in
every release from 2026-05-25 to 2026-08-20.

It also very nearly regressed again in v0.37.0. The chain is `deploy-web` ->
`web` -> `wasm` -> `docs` (`Justfile:377,370,362,321`), so cutting the release
rewrote the file 142 -> 0 in the working tree. It was caught only by reading
`git status` after the deploy and reverting by hand; a `git add -A` at that
moment would have committed the zeroed table with no diff anyone would have
questioned.

The `deploy-web` comment (`Justfile:372-375`) actively points the other way:
*"the artifacts it writes into `web/public/` are gitignored and must not be
committed."* True of `web/public/`, and it reads as a complete account of what
the deploy touches -- but `stdlib/docstrings.tur` is neither in `web/public/`
nor gitignored, and it is the one file the deploy reliably dirties.

## Fix directions

Roughly in order of cost, and the first two are independent of each other:

1. **Distinguish absent from empty, and refuse rather than clobber.** In
   `tools/gendocs.py:1764-1772`, when `--emit-tur` targets a path that already
   exists and the manifest is missing, either (a) parse the existing file's
   verified block and carry it forward, or (b) leave the block untouched, or
   (c) exit non-zero telling the caller to run `just doctest` first. (a) keeps
   `just docs` working standalone, which is probably what a contributor wants;
   (c) is the most honest but makes `docs` unrunnable without a build.
   Whichever, it must stop treating "no data" as "data saying no".
2. **Make the dependency real.** `docs: guides spices` -> `docs: guides spices
   doctest`, or a narrower `verified-manifest` recipe that the doctest run and
   `docs` both hang off. This is the actual bug in the pipeline: the consumer
   does not require its producer.
3. **Have the manifest record completeness, not just names.** A header line
   (`# complete: 108/108`) or a sentinel written only on a clean finish lets
   gendocs tell a truthful empty manifest from a truncated one, which
   `run-doctests.sh:63`'s unconditional truncate currently makes impossible.
4. **Guard it in CI.** Regenerate and `git diff --exit-code
   stdlib/docstrings.tur` in the web job, which would have caught every one of
   the flip-flops above the moment it landed.

A cheaper stopgap, if none of the above land soon: teach the release skill's
step 7 to name this file specifically, since it already says the tree should
stay clean through the deploy and this is the one thing that reliably dirties
it.

## Adjacent question, not investigated

Whether `doc-verified?` should exist as an exported predicate at all, given
nothing calls it and its value has been wrong for most of its life. Retiring it
is a smaller change than fixing the pipeline that feeds it. Recorded as a
question, not a recommendation -- the doctest work it belongs to
(`cf15a4281`, "unblock `tur run test`") only just landed, so it may be about to
get its first real consumer.

## Resolution (2026-08-20)

**(1) gendocs distinguishes absent from empty, and carries forward.**
`read_verified_manifest()` returns `(names, reason)` where `names` is `None`
for "no authoritative data" -- distinct from `set()`, "authoritatively nothing".
On `None`, `emit_docstrings_tur` recovers the list already embedded in the
output file (`read_existing_verified`) and re-emits it, printing
`142 verified carried forward` plus a stderr note naming the reason and the
command that refreshes it. A genuine complete-but-empty manifest still empties
the table, which is the point: the guard is against missing data, not against
bad news.

**(3) The manifest records completeness.** `run-doctests.sh` accumulates names
in a temp file and writes `verified.txt` once, at the end, behind a
`# complete: N passed, M failed, K skipped` header. gendocs trusts only a
manifest carrying that marker, so an interrupted run, a `^C`, or a manifest
written by the old append-as-you-go script all read as "no data" rather than
as a short list. This also fixed a second-order bug in the runner: it used to
truncate `verified.txt` *before* the "no generated test files" check, so
running it in a tree without generated tests destroyed a good manifest and
produced nothing.

Failures do not suppress the manifest -- the run publishes it and *then* exits
non-zero, because a failed case is legitimately not verified and that is a
complete verdict, not a missing one.

**(2) `docs: doctest` was declined.** It would put a build plus the full
doctest run in front of every docs regen, including the one inside
`deploy-web -> web -> wasm -> docs`. With (1) in place the destructive failure
mode is gone, so the ordering buys only freshness, and the cost is too high for
that. The relationship is documented in the `docs:` recipe comment instead:
run `just doctest` first if you want the table refreshed; without it the table
is preserved. `just test` depends on `doctest`, so anyone running the suite
refreshes it. The residual risk is now *staleness*, which is visible (gendocs
says "carried forward" every time) rather than silent.

**(4) The CI diff guard is still open.** It is now newly *viable*: the regen is
idempotent with respect to the verified table, so `git diff --exit-code
stdlib/docstrings.tur` after a regen would be stable and would have caught
every flip-flop in the table above. Not added here because turning it on is a
CI-policy call, and it would also start failing on ordinary un-regenerated
docstring edits -- which is arguably correct, but is a different conversation.

### Verified

All four manifest states, against the real `stdlib/docstrings.tur`:

| Manifest | Table | |
|---|---|---|
| absent | 142 -> 142 | file byte-identical to the committed one |
| present, no `# complete:` header | 142 -> 142 | does not shrink to the partial list |
| complete, 2 names | 142 -> 2 | authoritative, replaces |
| complete, 0 names | 142 -> 0 | a real "nothing passed" still empties it |

End-to-end: `python3 tools/doctest.py stdlib/ --out tests/doctest-generated/`
then `bash tools/run-doctests.sh` produced
`# complete: 161 passed, 5 failed, 258 skipped` with 144 unique names, and the
subsequent regen moved the tracked table 142 -> 144. The two additions
(`env/set`, `env/unset`) are names that pass here and were missing from the
committed table -- the first evidence, incidentally, that the committed table
was not merely empty-or-right but had also been *wrong* while non-empty.

### Residual: the verified set is machine-dependent

The same run reports 5 failures, all in `env`, and all because the docstring
examples are illustrative rather than assertive (`(env/user) ; => "alice"`).
They fail on any machine that is not a fictional Linux box belonging to alice,
so the "verified" set legitimately differs per machine -- and this file is
tracked. This fix does not address that; it is filed separately as
`docs/reported/env-doctests-are-machine-dependent.md`, which also covers the
larger consequence (those failures make `run-doctests.sh` exit non-zero, which
stops `just test` before ctest).
