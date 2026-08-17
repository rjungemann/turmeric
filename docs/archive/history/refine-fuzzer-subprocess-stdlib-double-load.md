# `tur` double-loads stdlib when spawned via a subprocess -> refine source fuzzer is vacuous

**RESOLVED 2026-07-26.** Root cause was `load_path_key` (`elab_toplevel.c`)
canonicalizing a `(load "stdlib/X")` via `realpath(path)` *before* the resolved
`stdlib_dir`: from a checkout root the cwd-relative `stdlib/X` realpath'd to the
CWD copy, while the auto-load prefix had resolved its stdlib to a *different*
dir (a stale installed one on `TUR_STDLIB_DIR`, e.g. mise's) -- so the dedup keys
disagreed and `map.tur`'s `(load "stdlib/hamt.tur")` re-spliced the already-
auto-loaded hamt. Fixed by resolving a `stdlib/<rest>` load through `stdlib_dir`
first. The fuzzer self-test now passes on a Debug `tur`. Two operational notes
that were *not* bugs: the fuzzer needs a **Debug** `tur` (Release strips
contracts under NDEBUG, so gate-off never aborts) and `TUR_STDLIB_DIR` unset (so
the repo compiler uses the repo stdlib). Original report below.

**Severity:** medium (blocks a test harness, not user programs; but any *programmatic*
`tur` invocation is affected, not just the fuzzer).

## Summary

`tests/refine-fuzz-src.py` (the refinement **source-level** differential fuzzer --
the only harness that reaches the encoder, per
`docs/guides/refinement-types-guide.md`) is currently **vacuous**: every case is
classified `skip_invalid` because the gate-off run *rejects at compile time*, so
the gate-off-aborts-vs-gate-on-clean differential never runs. The fuzzer's own
`--self-test` fails every pinned fixture with `off=reject on=reject`.

Root cause: when `tur` is spawned via a **Python subprocess** it loads its stdlib
**twice** and dies with, e.g.:

```
stdlib/hamt.tur:10:11: error: extern-c: 'tur_hamt_new' is already defined
stdlib/map.tur:943:11: error: defmacro: 'hamt-of' is already defined
```

The *same* invocation from a shell is clean.

## Reproduce

```python
import subprocess, os
R = os.getcwd()                       # a turmeric checkout root
p = subprocess.run([R+"/build/tur", "run",
                    "tests/fixtures/refine-proved/input.tur"],
                   capture_output=True, text=True, cwd=R)
print(p.returncode, [l for l in p.stderr.splitlines() if "already defined" in l])
# -> 1  ['stdlib/hamt.tur:10:11: error: extern-c: ...already defined', ...]
```

versus, identical binary / cwd / env, from the shell:

```sh
./build/tur run tests/fixtures/refine-proved/input.tur   # rc=0, clean
```

## Characterization (what does and does NOT change it)

- Happens for **both** `tur check` and `tur run`.
- Happens with **both** a Debug and a Release `tur`.
- **Not** env: reproduces with `env=None` (inherited), and with the fuzzer's
  `TUR_REFINE_STATS=1` / `ASAN_OPTIONS=detect_leaks=0` added or removed.
- **Not** the tur path: absolute vs relative argv[0] both reproduce under
  subprocess and both are clean under the shell.
- **Not** `--no-auto-spice`, and **not** a `/tmp/tur-build` cache (reproduces
  after `rm -rf /tmp/tur-build`).
- **Not** parallelism: reproduces with `--jobs 1`.
- **Not** `shell=True` vs a list argv (both subprocess forms reproduce).

So the trigger is specifically "spawned as a child process outside an
interactive shell", which points at stdlib *resolution* differing there --
likely loading the stdlib once cwd-relative (`stdlib/…` exists at the checkout
root) **and** once via the executable-location walk, when a shell invocation
somehow resolves to a single load. `tests/run.sh` uses a shell `$TUR`, which is
why the compiled-fixture suite is unaffected and this went unnoticed.

## Why it matters now

It blocks the soundness **gate** for the stateful-refinement (`#reads`) work
(`docs/archive/refine-stateful-measures-plan.md`): the plan requires a
`stateful` fuzzer shape + a sabotage run, and the fuzzer cannot run a single
meaningful case until this is fixed. See that plan's fuzzer note for the interim
targeted-sabotage evidence used in its place.

## Fix directions

Find where `tur` enumerates stdlib sources (the exe-location walk + any
cwd-relative `stdlib/` fallback) and make it load each module once regardless of
how it was invoked -- dedupe by resolved absolute path. A quick confirmation:
run the repro with a resolver trace (`TUR_DEBUG_RESOLVER=1`) under subprocess vs
shell and diff which `stdlib/*.tur` paths are opened.
