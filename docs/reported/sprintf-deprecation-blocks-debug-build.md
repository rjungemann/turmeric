---
title: sprintf deprecation blocks Debug build on macOS SDK 14+
severity: build-block (low; fix is one-line per call site)
---

# Summary

`cmake --build build --config Debug` fails on macOS with the Xcode 15+
SDK because two `sprintf` call sites in `src/main.c` are
`-Werror,-Wdeprecated-declarations` under the SDK's stricter
deprecation header. The compiler/codegen path itself is fine; this is
a host-toolchain compatibility gap.

# Observed

```
src/main.c:7324:5: error: 'sprintf' is deprecated: ...
 7324 |     sprintf(out, "%s.%s", base, key);
src/main.c:7330:5: error: 'sprintf' is deprecated: ...
 7330 |     sprintf(out, "%s[%lld]", b, (long long)idx);
```

# Root cause

`sch_mkpath` (line 7321) and `sch_mkidx` (line 7327) -- two schema
path-builder helpers -- use raw `sprintf` into a `malloc`ed buffer
whose size was computed by hand. The buffer size is correct, so the
deprecation is purely about the API surface, not a real overflow risk.

# Fix

Replace each `sprintf(out, ...)` with `snprintf(out, alloc_size, ...)`
where `alloc_size` is the same value passed to the preceding `malloc`.
Trivial; no behavior change.

# Validation

`bash tests/run.sh` passes after the fix (M2 audit verification ran
under it).

# Note

Fixed as a drive-by during M2 of the end-to-end-monomorphization plan
because it blocked the rebuild needed to validate the M2 codegen
change. Logged here so the next person who touches schema-path code
sees the history.
