# `TUR_STDLIB_DIR` is validated for existence, not for matching this compiler

**Status: RESOLVED 2026-09-02** by fix direction (1), the version stamp. Fix
direction (2) -- the heuristic notice -- landed 2026-08-28 and has now been
demoted to the fallback it should always have been; see the Resolution section
at the bottom.

**Severity: medium** -- not a miscompile in itself, but it routes a current
compiler at an old release's stdlib and the failure surfaces far downstream as
what looks exactly like a codegen bug. Found 2026-08-28 while resolving
[type-fuzz-src-red-on-clang-21](../archive/type-fuzz-src-red-on-clang-21.md),
which was entirely this and cost a full investigation to establish.

## Summary

`resolve_stdlib_root` (`src/main.c:263`) accepts `$TUR_STDLIB_DIR` if
`$TUR_STDLIB_DIR/macros.tur` is readable, and rejects it otherwise with a
warning that names the variable. A stdlib from a *different release* passes
that probe -- it has a perfectly readable `macros.tur`.

The guard's own comment (`src/main.c:269-282`) states the risk correctly:

> It is an ordinary environment variable, so it is inherited by anything a tur
> process spawns and it outlives the install that set it.

but then defends only against a stdlib that has "moved or been deleted". The
stale-but-intact case is the more common one -- a version manager keeps every
release on disk -- and it is the one that fails invisibly.

## What it costs

With `TUR_STDLIB_DIR` pointing at turmeric 0.36.0 while running a v0.40.0
compiler, `(defstruct Result [A B] (is-ok :bool) (ok-val A) (err-val B))` is
compiled against a current tagged-union box layout:

```
error: no member named 'is_ok' in 'tur_result_box_t'
    return (tur_adt_Result__int__int){.is_ok = __t172->is_ok, ...};
```

Nothing in that output names `TUR_STDLIB_DIR`, mentions a stdlib, or suggests
an environment problem. It reads as a Result-lowering bug in the compiler. The
archived report reached "every one of the 15 is a Result shape ... start at the
SR2b sum lowering" on exactly this evidence, which was a reasonable reading and
the wrong one.

The variable is also easy to acquire without knowing you have: mise's `python3`
is a shim that re-exports the active tool environment inside the process it
launches, so a shell with no `TUR_STDLIB_DIR` can spawn a process that has one.

## Repro

```sh
# any turmeric install of a different vintage
TUR_STDLIB_DIR=<other-release>/stdlib ./build/tur run some-program-using-result.tur
```

Observe a `tur_result_box_t` member error, with no diagnostic naming the
variable.

## Fix directions

1. **Stamp the stdlib with a version and compare it.** Requires adding the
   stamp (there is no version marker under `stdlib/` today) -- e.g. a
   `stdlib/VERSION` written from the top-level `VERSION` at build time, checked
   against the compiler's own version at resolve time. Mismatch becomes a
   warning naming the variable, or an error under a strictness flag.
2. **Cheaper stopgap: say what is being used when it is not the default.**
   When `$TUR_STDLIB_DIR` resolves to a directory other than the walk-up result
   for this binary, emit a one-line note to stderr. Needs care not to fire on
   legitimate installed layouts (`<exe_dir>/../share/turmeric/stdlib`), or it
   becomes noise everyone learns to ignore.
3. **Probe a shape the layout actually depends on** rather than a version
   string -- e.g. that `stdlib/result.tur` declares the arity/spelling this
   compiler expects. More robust than a version compare across dev builds, but
   more coupling.

(1) is the honest fix; (2) alone would have collapsed the investigation above
from hours to seconds and is worth having regardless.

## Guides to update when fixed

- docs/guides/troubleshooting-guide.md -- if a new diagnostic is added.

## Resolution (2026-09-02)

`stdlib/VERSION` is the stamp fix direction (1) asked for: written beside the
top-level `VERSION`, shipped with the stdlib, and read by `resolve_stdlib_root`
at startup and compared against `TUR_VERSION`. A mismatch names both versions
and the variable:

```
tur: TUR_STDLIB_DIR=/opt/turmeric/0.36.0/stdlib is turmeric 0.36.0, but this
     compiler is v0.42.2.
tur: a mismatched stdlib miscompiles in ways that name neither the stdlib nor
     this variable; unset it if that was not deliberate.
```

### The heuristic is now a fallback, which closes the other half of the report

The report's remaining complaint was not only that the mismatch went unchecked.
It was also that fix direction (2)'s notice is a heuristic -- "differs from the
walk-up" -- so "someone who deliberately points at another tree gets a notice
they do not need". That is real, and it showed up the moment the stamp existed:
with a stdlib in another directory whose version MATCHED, the old notice still
said "a stdlib from a different release will miscompile against this compiler",
which was now demonstrably false.

So the three verdicts are separated (`StdlibVerdict`):

- **MISMATCH** -- the definite message above. The heuristic is not consulted.
- **UNKNOWN** (no stamp) -- fall back to the heuristic, which now also says it
  found no stamp to check against. A stdlib without one predates this check, so
  it IS from an older release; the "differs from the walk-up" test is still the
  best available evidence there.
- **MATCH** -- silent, wherever the stdlib sits.

A confirmed match saying nothing is the part that makes the deliberate case
usable, and it is only possible because the stamp exists.

### Where the check does and does not fire

An unstamped stdlib is reported only when `TUR_STDLIB_DIR` supplied it. The
walk-up path stays silent on UNKNOWN: an odd or partial install layout should
not nag on every invocation, and the report's own note about the wasm/LSP
embedder (which sets the variable deliberately and has no exe path) applies. A
confirmed MISMATCH is reported on both paths -- on the walk-up it means a broken
or half-updated install rather than a shell problem, and the message says so.

It is a warning, not an error. Pointing a compiler at another tree's stdlib is a
legitimate deliberate act, and this runs before any flag could suppress it.

### The stamp has to be kept honest, and that is the real risk

A stamp that drifts is **worse than no stamp, because it is trusted**: a stale
`stdlib/VERSION` makes a genuinely mismatched stdlib look like a match AND makes
the correct one warn. `tests/check-stdlib-version-stamp.sh` (ctest
`tur_stdlib_version_stamp`) fails when the two files disagree, when either is
empty, or when the stamp is missing -- verified against all three. The three
`cut-*-release` commands bump `stdlib/VERSION` alongside `VERSION` and include it
in their `git add`; they already did this for two other files that mirror
`VERSION` (`wasm_glue.h`, `sw.js`), so the shape was established.

### A note on the guide this report names

"Guides to update when fixed" points at `docs/guides/troubleshooting-guide.md`,
which does not exist and never has -- confirmed against `origin/main`, not just
the working tree. The material went to
[docs/guides/tvm-guide.md](../guides/tvm-guide.md) instead, under **"When `tur`
says the stdlib does not match"**: tvm is the tool that SETS `TUR_STDLIB_DIR`,
so its guide is where someone hitting this is already looking, and the report's
own acquisition story (a version manager's `python3` shim re-exporting its tool
environment into a spawned process) is recorded there too.

### Verification

Suite 2751 passed / 0 failed, zero snapshot churn. All four paths exercised by
hand: stamped-and-different, unstamped, stamped-and-matching (silent), and no
variable at all (silent). `tests/run-stdlib-checks.sh` still 35/35 -- the new
`VERSION` file is not a `.tur` and no stdlib walk picks it up.
