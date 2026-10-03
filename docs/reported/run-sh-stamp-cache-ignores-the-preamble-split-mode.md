# `tests/run.sh`'s stamp cache ignores `TUR_PREAMBLE_SPLIT`, so an A/B of the two preamble paths reports green for a run that never happened

**Severity: medium.** No wrong answers in the compiler. The damage is that
`TUR_PREAMBLE_SPLIT=<n> bash tests/run.sh` reports **`N passed, 0 failed` for a
run that did not rebuild anything**, and the one task that most wants to run the
suite twice -- validating a preamble path before flipping its default -- is
exactly the task that hits it. A green that was never earned is worse than a red
one, because it is acted on.

Filed 2026-10-02 while measuring
[docs/upcoming/cc-path-preamble-split-plan.md](https://github.com/rjungemann/turmeric/blob/main/docs/upcoming/cc-path-preamble-split-plan.md)'s
"Left" item for macOS (rjungemann/turmeric#1023). Same class as the resolved
[run-sh-stamp-cache-ignores-the-stdlib](https://github.com/rjungemann/turmeric/blob/main/docs/archive/run-sh-stamp-cache-ignores-the-stdlib.md),
one input further out: that one was a *file* the stamp did not hash, this one is
an *environment variable that changes how every fixture is linked*.

## The finding

`tests/run.sh:539-546`:

```sh
stamp_key() {
    local input="$1"
    ...
    echo "$(_tur_hash_file "$input")-${ec_hash}-${TUR_MTIME}-${TUR_STDLIB_HASH}"
}
```

Four inputs: the fixture's `input.tur`, its `expected.c` snapshot, the `tur`
binary's mtime, and one hash over `stdlib/`. `TUR_PREAMBLE_SPLIT` is in none of
them, and neither is `TUR_CC_FLAGS` or `CC` (the harness comment at
`tests/run.sh:496-499` already admits the C compiler is uncovered).

So two runs that compile and link every fixture *differently* share one stamp
namespace. The second one PASS-skips the whole corpus from the first one's
stamps.

## Minimal repro

macOS, where the split is opt-in, so both modes are reachable from one build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j                      # all targets: needs libturt_preamble.a

TUR_PREAMBLE_SPLIT=1 bash tests/run.sh 2>&1 | tail -2   # real run
TUR_PREAMBLE_SPLIT=0 bash tests/run.sh 2>&1 | tail -2   # a no-op that claims green
```

Measured on an 8-core arm64 Mac, 3489 fixtures:

| run | wall | summary |
| --- | --- | --- |
| `TUR_PREAMBLE_SPLIT=1`, cold stamps | 7:06 | `3489 passed, 0 failed` |
| `TUR_PREAMBLE_SPLIT=0` immediately after | **1:14** | `3489 passed, 0 failed` |

A quarter of that corpus under `TUR_FORCE=1` takes 112s on the same box, so a
74-second full run is not a fast run -- it is 3489 stamp hits. Nothing was
compiled with the whole preamble, and nothing says so.

The direction that matters is the same in reverse: run the inline suite first
and the `TUR_PREAMBLE_SPLIT=1` run that is supposed to qualify a new platform
skips every fixture and signs off on a path it never took.

## Root cause

`stamp_key` (`tests/run.sh:539`) keys on fixture content and compiler identity,
not on the *configuration* the fixture is built under. `TUR_PREAMBLE_SPLIT`
reaches `preamble_split_auto_applies()` (`src/main.c:974`) and decides whether
the emitted TU carries the 4400-line runtime preamble or a decls region plus
`-lturt_preamble`. That is a different compile and a different link for every
fixture in the corpus, and it is invisible to the stamp.

The `tur` mtime input does not save it, because flipping the variable does not
rebuild `tur`.

## Why CI does not catch it

Each CI job checks out fresh, so `tests/.stamp-cache/` starts empty and the
`test` / `whole-preamble` legs are each genuinely cold. The trap is local-only
-- which is also where the plan tells a human to run the qualifying suite
("Run `TUR_PREAMBLE_SPLIT=1 bash tests/run.sh` once on a Mac").

## Fix directions

1. **Add the mode to the key.** One term, mirroring how `TUR_STDLIB_HASH` was
   added: fold `TUR_PREAMBLE_SPLIT` (and, for the same reason, `TUR_CC_FLAGS`
   and `CC`) into `stamp_key`. Cheap, and it makes the two paths share a build
   tree safely.
2. **Namespace the cache per mode.** `TUR_STAMP_CACHE="tests/.stamp-cache-split"`
   when the split is on. Fewer invalidations than (1), but it leaves
   `TUR_CC_FLAGS` uncovered.
3. **At minimum, say so.** Print the resolved preamble mode in the run header
   and extend the `tests/run.sh:496-499` comment, which already lists what the
   stamp does not cover, to name this.

(1) is the fix. (3) is worth doing regardless: the harness currently gives no
indication of which preamble path a run took, so a reader of the summary line
cannot tell these two runs apart.
