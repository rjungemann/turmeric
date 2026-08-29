# `TUR_STDLIB_DIR` is validated for existence, not for matching this compiler

**PARTIALLY FIXED 2026-08-28 -- fix direction (2) only; this report stays open
for (1).** `resolve_stdlib_root` now emits a two-line notice when
`$TUR_STDLIB_DIR` is accepted *and* a stdlib beside the binary exists *and* the
two are different directories (`realpath`-compared, so a symlinked or
trailing-slash spelling does not false-positive):

```
tur: TUR_STDLIB_DIR=<...>/turmeric/0.36.0/stdlib overrides the stdlib beside this binary (<...>/stdlib).
tur: a stdlib from a different release will miscompile against this compiler; unset it if that was not deliberate.
```

That is the diagnosability half, and it is what would have collapsed the
investigation behind `type-fuzz-src-red-on-clang-21` from hours to seconds.

**The validation half is still missing**, which is what this report is about: a
mismatched stdlib is still *accepted*, and the notice is a heuristic ("differs
from the walk-up"), not a check that the stdlib matches this compiler. Someone
who deliberately points at another tree gets a notice they do not need, and a
compiler with no walk-up stdlib beside it (an odd install layout) still gets no
signal at all. Closing it properly needs fix direction (1) below -- a version
stamp -- which does not exist yet.

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
