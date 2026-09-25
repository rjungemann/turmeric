---
title: tests/run.sh's stamp cache ignores the stdlib, so a stdlib-only change is reported green without being run
category: Archive
description: stamp_key is hash(input) + hash(expected.c) + mtime(tur). Editing stdlib/*.tur invalidates nothing, so every fixture PASS-skips from cache and the summary reports a full green run that never recompiled anything. Cost a full CI round on PR #909.
---

# `tests/run.sh`'s stamp cache ignores the stdlib

> **RESOLVED 2026-09-25.** Fix direction 1: `tests/run.sh` hashes every file
> under `stdlib/` once at startup (`TUR_STDLIB_HASH`, ~0.2s) and appends it
> to `stamp_key`, so a stdlib-only edit invalidates every stamp.  The harness
> comment now says what the stamp covers and what it still does not (a
> `load` from outside `stdlib/` and the fixture's own directory, and the C
> compiler).


**Severity: medium.** No wrong answers in the compiler -- but the suite reports
**`N passed, 0 failed` for a run that did not happen**, which is worse than a
red result because it is acted on.

**Status:** OPEN. Filed 2026-09-18 after it cost a CI round on PR #909.

## The finding

`tests/run.sh`:

```sh
stamp_key() {
    ...
    echo "$(_tur_hash_file "$input")-${ec_hash}-${TUR_MTIME}"
}
```

Three inputs: the fixture's `input.tur`, its `expected.c`, and the **mtime of
the `tur` binary**. `stamp_check` PASS-skips the fixture when all three match
the last passing run.

**The stdlib is not among them.** `stdlib/*.tur` is data the compiler reads at
elaboration time, not something linked into `tur`, so editing it changes
neither the binary's mtime nor any fixture file. Every stamp stays valid and
every fixture is skipped.

## Repro

1. `bash tests/run.sh` once, to populate the stamp cache.
2. Edit any `stdlib/*.tur` in a way that changes emitted C -- for the observed
   case, `(defn json/bool [v : int]` -> `[v : bool]`.
3. `bash tests/run.sh` again, WITHOUT rebuilding `tur`.

Observed: `summary: 3047 passed, 0 failed`, in a fraction of the usual time.
Nothing was recompiled.

CI, which has no stamp cache, ran the same tree and reported **148 codegen
mismatches** in `Test (ubuntu-latest)` / `Test (macos-latest)` plus a failing
`Check codegen snapshots` guard -- all of them the one-word signature change
propagating into every fixture that pulls in `stdlib/json.tur`.

## Why it is easy to miss

The failure is silent and the summary is the reassuring shape. It also does not
reproduce for anyone who happened to rebuild `tur` in the same session: a
compiler change bumps `TUR_MTIME` and invalidates everything, so the trap only
springs on a **stdlib-only** edit -- exactly the shape of a stdlib audit, a
docstring pass, or a signature tightening.

This is a sibling of the traps already in
[CLAUDE.md](../../CLAUDE.md) under "Overlapping runs cause false FAILURES":
there the lesson is that assertions which pass when run by hand may never have
run at all. Same lesson, opposite direction.

## Workaround

`TUR_FORCE=1 bash tests/run.sh` bypasses `stamp_check` entirely. Use it after
any stdlib edit, or delete the stamp cache.

## Fix directions

1. **Fold the stdlib into the key.** A single hash over `stdlib/*.tur` computed
   once at startup (like `TUR_MTIME`) and appended to every `stamp_key`. Cheap:
   one pass over ~60 files per run, not per fixture.
2. Or hash whatever the fixture actually `load`s, which is narrower but needs
   the load graph and is not obviously worth it over (1).
3. Whichever lands, `tests/run.sh --help` and the harness comment should say
   what the stamp does and does not cover, since "unchanged since the last
   passing run" currently reads as stronger than it is.
