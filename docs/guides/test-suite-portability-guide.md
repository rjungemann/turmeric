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
carry this guard; keep it in place, and copy the pattern into any new
terminal-state code.

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
    timeout 300 ctest -j --output-on-failure --progress --test-dir build
```

Under parallel `ctest`, end-to-end wall time is bounded by the slowest
single target (usually `tests/run.sh` cold), not the sum. Removing the
`-j` and shipping is a soft regression that will not show up in any test's
own timing -- watch for it in code review.

---

## 6. Timeout budget

`bash tests/run.sh` is expected to complete in ~4-5 minutes end-to-end and
**must** always be invoked with a 10-minute (`timeout: 600000`) budget --
see the top-level `CLAUDE.md` "Test Suite Timeout" rule. If a run stretches
to 15-20 minutes, suspect CPU contention (overlapping suite runs), not a
hang: per-fixture *run* timeouts already cap at 10s, so a genuine runtime
loop surfaces as `FAIL`, not an indefinite stall.

---

## See also

- [test-runner-contract.md](test-runner-contract.md) -- stdlib test
  framework contract (assertions, discovery, exit semantics).
- [performance-guide.md](performance-guide.md) -- user-facing performance
  guidance for Turmeric programs (not the test harness itself).
