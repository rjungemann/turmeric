---
title: Test Suite Portability & Performance Guide
category: Contributor
description: macOS / Bash 3.2 gotchas, background-terminal SIGTTOU guards, inline-C math wrapper shadowing, and stamp-cache patterns for the test harnesses
---

# Test Suite Portability & Performance Guide

The Turmeric test harnesses (`tests/run.sh`, `tests/run-turi.sh`,
`tools/run-doctests.sh`) run on both Linux CI and macOS developer boxes.
macOS ships Bash 3.2 by default, which trips a handful of portability
pitfalls that are easy to reintroduce. This guide records the gotchas that
have burned us, the patterns that fix them, and the stamp-cache design that
keeps parallel fixture runs cheap.

If you are editing anything under `tests/` or `tools/run-doctests.sh`,
skim this before you push.

---

## 1. macOS Bash 3.2 -- no `mapfile`

Bash 3.2 does not have `mapfile` (a.k.a. `readarray`). Any harness that
uses it silently produces empty arrays and turns real diagnostics into
false failures.

Use a portable `while read` loop instead:

```bash
expected_arr=()
while IFS= read -r line || [ -n "$line" ]; do
  expected_arr+=("$line")
done < "$expected_file"
```

The `|| [ -n "$line" ]` clause is what handles a final line without a
trailing newline -- do not drop it.

Rule: **no `mapfile` in any file under `tests/` or `tools/`.** Grep for it
before landing a patch.

---

## 2. Background jobs and `tcsetattr` -- guard against `SIGTTOU`

When a fixture runs under `xargs` (or any background subshell) and its
program touches terminal state -- `tcsetattr`, `stty`, raw mode -- the
kernel sends `SIGTTOU` to the background process group and stops it. From
the harness's point of view the fixture just hangs forever with no output.

Any inline-C that calls `tcsetattr` must first check whether the caller
actually owns the controlling terminal, and no-op otherwise:

```c
#include <unistd.h>
pid_t fg = tcgetpgrp((int)fd);
if (fg == -1 || fg != getpgrp()) {
  /* Backgrounded or not a tty -- skip; avoids SIGTTOU stop. */
  return -1;
}
/* ... tcsetattr(fd, ...) ... */
```

The `stdlib/term.tur` helpers (`term/set-raw`, `term/set-cooked`) already
carry this guard -- in `term/set-mode`, the single inline-C body both of
them delegate to; keep it in place, and copy the pattern into any new
terminal-state code.

The guard is also why terminal-state code is hard to *test*: it makes both
helpers no-op under the harness, which always redirects stdout.
`tests/fixtures/term-raw-cooked-roundtrip` gets past it by building a
terminal rather than borrowing one -- `posix_openpt`, then `fork` +
`setsid` + `ioctl(TIOCSCTTY)` in the child so the pty slave is a
controlling terminal whose foreground process group is the child's. Reuse
that shape for anything else that has to exercise a real tty.

---

## 3. Inline-C name shadowing -- do not call `sqrt` from a `defn sqrt`

A Turmeric `defn` compiles to a `static` C function of the same name.
Inside that function's inline-C body, an unqualified call to `sqrt(x)`
resolves to the local static function -- not `libm` -- and recurses until
the stack overflows. The fixture appears to hang.

The fix is to route math wrappers through the compiler's `__builtin_*`
intrinsics, which resolve to the target math op directly and cannot be
shadowed by a local static:

```turmeric
(defn sqrt [x : float] : float
  ```c
  return __builtin_sqrt(x);
  ```)

(defn fabs [x : float] : float
  ```c
  return __builtin_fabs(x);
  ```)
```

Both GCC and Clang support `__builtin_sqrt`, `__builtin_fabs`,
`__builtin_floor`, `__builtin_ceil`, `__builtin_pow`, etc. This is the
canonical pattern for any Turmeric wrapper whose name collides with a
libm symbol.

If you must call libm by its real name, rename the Turmeric-side wrapper
(e.g. `float/sqrt`) so the C symbol does not shadow libm.

---

## 4. Stamp cache -- cache `$TUR` mtime **once**, export to workers

`tests/run.sh` and `tests/run-turi.sh` compute a stamp key per fixture to
skip already-built outputs. The stamp key mixes the fixture hash with the
compiler binary's mtime, so a recompile of `tur` invalidates every cached
fixture -- correct, but naive implementations `stat` the binary once per
fixture (~1500 spawns) and once per parallel worker.

Cache the mtime in the parent shell and export it so `xargs`-spawned
workers inherit it:

```bash
_tur_mtime() {
  stat -f '%m' "$1" 2>/dev/null || stat -c '%Y' "$1" 2>/dev/null || echo "0"
}

export TUR_MTIME="$(_tur_mtime "$TUR")"

stamp_key() {
  local input="$1"
  local dir; dir="$(dirname "$input")"
  local ec_hash=""
  [ -f "$dir/expected.c" ] && ec_hash="$(_tur_hash_file "$dir/expected.c")"
  echo "$(_tur_hash_file "$input")-${ec_hash}-${TUR_MTIME}"
}
```

Two things to preserve when editing:

1. **Dual-flavour `stat`** -- BSD (`-f '%m'`) first, GNU (`-c '%Y'`)
   second, `0` fallback last. Do not collapse to one flavour.
2. **`export` the cached value.** Without `export`, `xargs` workers do not
   inherit `TUR_MTIME` and silently re-stat per fixture.

The same pattern applies to `tools/run-doctests.sh`; the doctest runner
does not need the fixture's `expected.c` hash but must still cache
`TUR_MTIME` once.

---

## 5. Parallel `ctest`

Root-level `ctest` invocations must pass `-j` so the ~68 registered
targets run across cores rather than sequentially. The Justfile recipe
looks like:

```justfile
test: build doctest
    timeout 720 ctest -j "$(getconf _NPROCESSORS_ONLN)" --output-on-failure --progress --test-dir build
```

**The job count must be explicit.** A bare `ctest -j` looks like it asks for
"parallel, pick a number" -- it does not. Through CMake 3.28 the option is
documented as `-j <jobs>`; a value-less `--parallel` only arrived in 3.29.
On 3.28 a bare `-j` is accepted **in silence and does nothing**: measured on a
4-core box, five targets took 11.2s serial, 11.5s with bare `-j`, and 5.8s
with an explicit count. So a bare `-j` is the worst of both worlds -- it reads
in review as the parallel recipe while behaving exactly like the serial one,
which is how this section's own snippet came to document a no-op. Use
`getconf _NPROCESSORS_ONLN`: `nproc` is GNU coreutils only, and macOS has
neither it nor a value-less `-j` on the CMake it ships.

The cap is 12 minutes, not 5: `tests/run.sh` alone is ~265s on a 4-core box
and is `RUN_SERIAL`, so a 300s cap kills the whole suite on any machine
slower than the one it was tuned on -- and the kill looks like a hang, not a
timeout. 12 minutes is the repo-wide suite timeout; see CLAUDE.md.

Under parallel `ctest`, end-to-end wall time is bounded by the slowest
single target (usually `tests/run.sh` cold), not the sum. Removing the
`-j` and shipping is a soft regression that will not show up in any test's
own timing -- watch for it in code review.

`-j` is safe here only because the heavy targets are marked `RUN_SERIAL`
(`tur_tests`, `turi_fixture_tests`, `tur_jit_fixture_tests`): each already
fans out across `nproc` internally, so letting ctest run two of them at once
oversubscribes the box and expires their per-fixture timeouts. Keep the
marking when you add a fan-out harness -- see "Failures That Are Not Product
Bugs" in [test-runner-contract.md](test-runner-contract.md).

---

## 6. A check that enumerated nothing passes vacuously

`head -z` / `--zero-terminated` is a GNU coreutils extension; BSD/macOS
`head` does not have it. In `tests/run-fmt.sh` it errored, the
NUL-separated `read` loop never ran, the failure counter stayed at zero,
and `fmt-idempotence-stdlib` reported PASS having checked **zero files**.
Formatter idempotence had no macOS coverage at all and the summary line
said everything was fine.

The general shape: **any check whose `pass` is guarded only by "no
failures accumulated" is greenest when its enumeration breaks.** Every
file-walking check needs an explicit "checked at least one file" guard:

```bash
SEEN=0
while IFS= read -r -d '' f; do
    SEEN=$((SEEN + 1))
    ...
done < <(find stdlib -name '*.tur' -print0)

if [ "$SEEN" -eq 0 ]; then
    fail "$NAME" "no files checked -- stdlib enumeration produced nothing"
elif [ "$FAILED" -eq 0 ]; then
    pass "$NAME"
fi
```

Bound a sample inside the loop (`[ "$SEEN" -ge 20 ] && break`) rather than
with a `head` in the pipeline. `tests/run-fmt.sh` carries both guards
(`fmt-bootstrap-stdlib`, `fmt-idempotence-stdlib`) -- copy them.

---

## 7. Heap probes and sanitizers do not mix

A malloc-probe assertion means nothing under ASan, and it means nothing
*differently* per platform:

- **glibc** -- ASan replaces the allocator, so `mallinfo2().uordblks`
  reads 0 and the check is vacuously green.
- **Darwin** -- the probe reads ASan's own zone and the free quarantine
  inflates it (measured: 160 bytes per iteration of quarantined frees).

The two platforms disagree, which is worse than both being wrong: the
glibc leg looks like a working control for the Darwin leg.

Separately, `malloc_zone_statistics(malloc_default_zone(), ...)` on Darwin
measures the **whole default zone**, including stdlib bucket/page
bookkeeping, and moves in 16-32 KB steps. Absolute heap-delta assertions
are noise there; glibc's `mallinfo2().uordblks` is the narrow equivalent.

Two rules fall out:

1. The diagnostic that settles "leak or noise" is **scaling**: a real leak
   grows with iteration count, noise does not. Probe at two sizes before
   believing a number.
2. `mallinfo2` is useless anywhere in this tree that links an
   ASan-instrumented `libturi`. Use RSS from `/proc/self/statm`, which is
   allocator-independent.

---

## 8. Harness environment parity

`tests/run.sh` exports `TUR_BIND_LOOPBACK=1`; `stdlib/httpd.tur` and
`stdlib/async_socket.tur` read it at run time and bind `INADDR_LOOPBACK`
instead of `INADDR_ANY`. A sibling harness that forgets the export makes
every server fixture bind all interfaces.

That is not a cosmetic difference. It surfaced as an apparent macOS-only
JIT defect: BSD permits a wildcard bind while a specific address holds the
port, so the second server silently succeeded and stole nothing on Linux
(which refuses the wildcard-over-specific bind) but did on macOS. The
mechanism was BSD socket semantics; the cause was one missing `export` in
`tests/run-jit.sh`.

Rule: **any env var `tests/run.sh` exports, every sibling harness must
export.** Diff them before landing a new runner:

```sh
grep -n TUR_BIND_LOOPBACK tests/run.sh tests/run-jit.sh
```

---

## 9. String literals are not reliably merged

C11 6.4.5p7 leaves it **unspecified** whether identical string literals in
one translation unit share an address. gcc and clang merge them; c2mir
(the JIT backend) does not.

A fixture that probes a map with the *same* literal it inserted therefore
tests pointer identity on one engine and content equality on another:

```c
/* gcc: MERGED (a == b)          c2mir: DISTINCT (a != b) */
```

So a JIT-only failure on a `cstr`-keyed container can be a **fixture**
defect, not an engine defect. Build the probe key separately from the
insert key -- or compare by content -- before filing anything against the
backend.

---

## 10. Timeout budget

`bash tests/run.sh` is expected to complete in ~4-5 minutes end-to-end and
**must** always be invoked with a 12-minute (`timeout: 720000`) budget --
see the top-level `CLAUDE.md` "Test Suite Timeout" rule. If a run stretches
to 15-20 minutes, suspect CPU contention (overlapping suite runs), not a
hang: per-fixture *run* timeouts already cap at 10s, so a genuine runtime
loop surfaces as `FAIL`, not an indefinite stall.

---

## See also

- [test-runner-contract.md](test-runner-contract.md) -- stdlib test
  framework contract (assertions, discovery, exit semantics), plus
  "Failures that are not product bugs" (sanitizer-laundered crashes,
  overlapping runs, contract fixtures under Release).
- [performance-guide.md](performance-guide.md) -- user-facing performance
  guidance for Turmeric programs (not the test harness itself).
