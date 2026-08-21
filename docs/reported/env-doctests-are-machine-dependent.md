# `stdlib/env.tur` doctests assert the author's machine, so `just test` never reaches ctest

**Summary.** Five `;;;` examples in `stdlib/env.tur` are *illustrative*
(`(env/user) ; => "alice"`), but `tools/doctest.py` turns every `; =>` into an
assertion. They can only pass on a machine whose `$HOME` is `/home/user`, whose
user is `alice`, whose `$SHELL` is `/bin/bash`, and whose `$PATH` is exactly
`/usr/bin:/bin:/usr/local/bin`. On any real machine they fail, `run-doctests.sh`
exits 1, and because `test: build doctest` (`Justfile:37`) a non-zero exit there
stops the chain -- so `just test` / `tur run test` does **not** reach ctest and
the suite does not run.

**Severity: medium.** Not a compiler defect, but it silently disables the test
suite for anyone who is not the fixture's imaginary user, and it does so in the
shape that reads like success: the recipe exits quickly, having run 5 failing
doctests and no tests. This is precisely the trap
`docs/archive/tur-run-test-blocked-by-doctest-failures.md` was filed for; that
report was resolved by `cf15a4281` ("unblock `tur run test` -- it now reaches
ctest and passes 108/108"), which is true on a machine where these five happen
not to run, and false here.

## Repro

```sh
$ python3 tools/doctest.py stdlib/ --out tests/doctest-generated/
$ bash tools/run-doctests.sh; echo "exit=$?"
FAIL env:env/get! -- got "/Users/rjungemann", expected "/home/user"
FAIL env:env/home -- got "/Users/rjungemann", expected "/home/user"
FAIL env:env/path -- got "/Users/rjungemann/.local/share/mise/shims:...", expected "/usr/bin:/bin:/usr/local/bin"
FAIL env:env/user -- got "rjungemann", expected "alice"
FAIL env:env/shell -- got "/bin/zsh", expected "/bin/bash"

Doctest results: 161 passed, 5 failed, 258 skipped
exit=1
```

`just test` then stops before ctest, exactly as `run-doctests.sh`'s own closing
NOTE warns it will.

## Root cause

`stdlib/env.tur:50,153,169,185,201` -- the five examples:

```turmeric
;;;   (env/get! "HOME")  ; => "/home/user"
;;;   (env/home)  ; => "/home/user"
;;;   (env/path)  ; => "/usr/bin:/bin:/usr/local/bin"
;;;   (env/user)  ; => "alice"
;;;   (env/shell)  ; => "/bin/bash"
```

These are good *documentation* -- a reader wants a concrete-looking value --
and they are the docstring standard's required `Example:` block doing its job.
The defect is the collision between two roles the `; =>` marker now carries:
"here is what this looks like" and "assert this". `tools/doctest.py` cannot
tell them apart, so a documentation convention became a test oracle for a
function whose entire purpose is to return environment-dependent values.

## Fix directions

1. **Give the extractor an opt-out marker** -- e.g. `; ~>` for "illustrative,
   do not assert", or a `;;; Doctest: skip` line in the block. Cheapest, keeps
   the docs readable, and generalizes: `env` will not be the last module whose
   honest answer is "it depends". This is the recommended one.
2. **Rewrite the five examples to assert something stable** --
   `(cstr-len (env/home))  ; => ...` does not work either; realistically it
   means asserting shape, not value, which the extractor cannot express today.
   Some would become untestable and should then use (1) anyway.
3. **Let `run-doctests.sh` carry a known-environment-dependent list** and count
   those as SKIP rather than FAIL. Works, but encodes the exception in the
   runner rather than at the docstring, where the fact lives.

Worth deciding alongside whichever of these lands: whether a doctest failure
should stop `just test` at all, or whether the doctests should run *after*
ctest so a documentation-example mismatch cannot mask the actual suite. The
current ordering means the least load-bearing check gates the most load-bearing
one.

## Adjacent

The same run is what populates `doc-verified?`
(`docs/archive/docstrings-verified-table-zeroed-by-regen.md`), so these five
names are absent from that table on this machine and present on one where they
pass -- i.e. the tracked `stdlib/docstrings.tur` has a machine-dependent
component. Small today (5 of ~150 names), and it argues for (1) or (3) over
leaving them failing.
