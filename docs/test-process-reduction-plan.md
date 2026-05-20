# Test Suite Process Reduction Plan

> **Status:** Tier 1 done; Tier 2 done (run.sh); Tier 3 infrastructure done (opt-in, `TUR_WORKER_POOL=1`)
>
> **Problem:** Running `just test` causes a massive CPU spike on macOS because
> `syspolicyd` (Gatekeeper) is flooded with 639+ new binary validations in
> rapid succession.
>
> **Last updated:** 2026-05-19

---

## Root Cause

`tests/run.sh` runs every happy-path fixture by:

1. Calling `tur build input.tur -o <tmpexe>` -- this internally calls `cc` to
   compile the emitted C, producing a **fresh binary on every run**.
2. Running that fresh binary.

On macOS, `syspolicyd` validates every newly-spawned executable. With 639 happy
fixtures and a default worker count of `nproc×2` (e.g. 20 on a 10-core Mac),
this creates a burst of 1,200+ process spawns and 639 simultaneous Gatekeeper
queries. The daemon cannot keep up; the kernel queues pile up; CPU pegs.

Per-fixture process accounting (current):

| Step | Processes | syspolicyd hit? |
|---|---|---|
| `tur emit-c` (codegen fixtures only) | 1 | No (trusted binary) |
| `tur build` → calls `cc` | 1--2 | No (`cc` is trusted) |
| Freshly compiled `<tmpexe>` | 1 | **YES** (new, unsigned binary) |

Total for 639 happy + 161 error fixtures: **~2,100 process spawns, 639 syspolicyd hits**.

---

## Fix Tiers

### Tier 1 -- Quick win: cap `TUR_TEST_JOBS` (30 min)

**Change the default from `nproc×2` to `nproc` (physical cores only).**

In `tests/run.sh`, replace:

```bash
JOBS=$(( _nproc * 2 ))
```

with:

```bash
JOBS=$(( _nproc ))
```

Also hard-cap at 8 instead of 32, or expose a `TUR_MAX_JOBS` env var so CI can
override independently of local runs.

**Impact:** Halves the burst process count with a one-line change. Does not fix
the root cause but meaningfully reduces the spike.

---

### Tier 2 -- Core fix: replace `tur build`+run with `tur run` (2--4 hours)

**Use `tur run` (interpreter mode) instead of compiling a native binary to
verify fixture output.**

The insight: the fixture test suite is checking language semantics (correct
output, correct exit code, correct diagnostics). It does not need a native
binary for that. `tur run input.tur` produces the same observable output as
the compiled binary for all fixtures that don't exercise compiled-only ABI
surface.

#### Changes to `tests/run.sh`

Split `run_happy` into two paths based on whether the fixture needs codegen
verification:

**Non-codegen fixtures** (no `expected.c`):

```bash
# Before
CC="$BUILD_CC" "$TUR" $fixture_flags build "$input" -o "$exe"
_run_timed "$fixture_timeout" "$exe" > "$actual_stdout" 2>> "$actual_stderr"

# After
_run_timed "$fixture_timeout" "$TUR" $fixture_flags run "$input" \
    > "$actual_stdout" 2>> "$actual_stderr"
```

Zero `cc` invocations. Zero new binaries. Zero syspolicyd hits.

**Codegen fixtures** (have `expected.c`):

```bash
# emit-c for the snapshot diff (already done)
"$TUR" $fixture_flags emit-c "$input" > "$actual_c"

# Then run via interpreter for stdout/exit-code check
_run_timed "$fixture_timeout" "$TUR" $fixture_flags run "$input" \
    > "$actual_stdout" 2>> "$actual_stderr"
```

Still zero `cc` invocations. The C snapshot diff still works; the stdout check
uses the interpreter.

#### Opt-out escape hatch

Some fixtures may depend on compiled-only behavior (e.g. exact C ABI layout
tests, async fiber stack behavior). Add an opt-out marker:

```
tests/fixtures/<name>/requires.compiled
```

When this file is present, `run_happy` falls back to the old `tur build` + run
path. Initially zero fixtures should need this; add as exceptions are discovered.

#### Impact

| Metric | Before | After Tier 2 |
|---|---|---|
| `cc` invocations | 639 | 0 (unless `requires.compiled`) |
| New binaries spawned | 639 | 0 (unless `requires.compiled`) |
| syspolicyd hits | 639 | 0 (unless `requires.compiled`) |
| Processes per fixture | ~3 | 1 |
| Total process spawns | ~2,100 | ~800 |

---

### Tier 3 -- Long-term: persistent worker pool (1--2 days) ✓ infrastructure done

Even with Tier 2, each fixture still spawns a fresh `tur` process. For 800
fixtures that is 800 process spawns total -- acceptable, but still measurable
on slow CI machines.

**Add a `tur worker` subcommand that reads fixture paths from stdin and
evaluates them in-process, returning structured results to stdout.**

```
echo "tests/fixtures/arith/input.tur" | tur worker
# stdout: {"name":"arith","status":"pass","stdout":"42\n","stderr":"","exit":0}
```

`tests/run.sh` then pipes the fixture list to a small pool of `N` persistent
`tur worker` processes (where `N` = physical core count). syspolicyd checks
`tur` exactly `N` times at pool startup; all 639 fixtures execute in those
same `N` processes.

#### Process accounting after Tier 3

| What | Count | syspolicyd hits |
|---|---|---|
| Worker pool startup | N (e.g. 8) | 8 |
| Per-fixture work | 0 new processes | 0 |
| Error fixtures (emit-c) | 161 `tur emit-c` calls | 0 (trusted binary) |
| **Total** | **~170** | **8** |

#### Worker protocol (sketch)

```
stdin line:  <fixture-path>\t<flags>\t<timeout>\n
stdout line: <json-result>\n
```

Isolation: each fixture evaluation resets the interpreter state (symbol table,
effects stack, heap). The worker catches panics/signals per-evaluation so a
crashing fixture doesn't kill the pool member (it is restarted by the
coordinator).

#### Shell-side coordinator (sketch)

```bash
# Start pool
for i in $(seq 1 "$JOBS"); do
    mkfifo "$RESULTS_DIR/req.$i" "$RESULTS_DIR/res.$i"
    "$TUR" worker < "$RESULTS_DIR/req.$i" > "$RESULTS_DIR/res.$i" &
    echo $! >> "$RESULTS_DIR/pids"
done

# Dispatch fixtures round-robin
for dir in "${HAPPY_DIRS[@]}"; do
    echo "$dir" > "$RESULTS_DIR/req.$((ordinal % JOBS + 1))"
done
```

#### Implementation notes (as shipped)

`tur worker` reads fixture directory paths from stdin (one per line). For each
fixture it:

1. Checks `requires.compiled` / `requires.tsan` markers and skips if needed.
2. Reads `flags`, `expected.timeout`, and `input.stdin` from the fixture dir.
3. Forks a child that: applies fixture flags to global compiler state, runs
   `turi_eval_file` with stdout/stderr captured via pipes, then calls `(main)`
   if a `main` function was defined. The fork gives crash isolation: a panicking
   fixture exits the child, not the worker.
4. If `expected.c` exists, forks a separate child to run `compile_to_c` and
   compare the generated C against the snapshot.
5. Compares actual stdout, exit code, and stderr substrings against expected
   files and writes a 4-line result file to `$RESULTS_DIR`.

`tests/run.sh` uses a round-robin batch dispatch: fixtures are split into N
files, one persistent `tur worker` per file runs in parallel, stamps are written
by the coordinator after all workers complete.

**Current status:** opt-in (`TUR_WORKER_POOL=1`). The interpreter does not yet
handle all language constructs used by fixture programs (notably `defdata` +
`match` inside `defn main`). Enable by default once interpreter parity is
achieved. Divergences between worker and compiled output are real interpreter
bugs.

---

## Recommended Implementation Order

1. **Tier 1** immediately -- one-line change, zero risk.
2. **Tier 2** next sprint -- eliminates 99% of the syspolicyd load.
3. **Tier 3** as a follow-up if CI times are still a concern after Tier 2.

Tier 2 alone should reduce total process spawns from ~2,100 to ~800 and drop
syspolicyd hits from 639 to 0 for the vast majority of runs.

---

## Notes

- The stamp-file cache (`tests/.stamp-cache/`) already skips unchanged
  fixtures. This complements but does not replace Tier 2: on a first run or
  after `TUR_FORCE=1`, all 639 fixtures fire.
- `TUR_EMIT_C_MODE=always` forces codegen for every fixture. Under Tier 2 this
  still avoids the `cc` + binary step; only `tur emit-c` and `tur run` run.
- TSAN fixtures (`requires.tsan`) always need compiled mode and will naturally
  land in the `requires.compiled` bucket.
- The `tur run` interpreter and the compiled path must produce identical output
  for the fixture suite to remain meaningful. Any divergence found during Tier 2
  migration is a real interpreter bug, not a test infrastructure problem.
