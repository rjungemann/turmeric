# The Windows JIT prelude and `mangle.c`'s `libc_names` can drift apart silently

**Severity:** low today, latent -- and the failure mode is a **wrong answer, not
a build error**, which is what earns it a tracker rather than a comment.

**Status:** open. Split out 2026-09-10 from
[jit-win-prelude-shadows-user-fn](../archive/jit-win-prelude-shadows-user-fn.md)
when that report was archived: it was resolved at filing, but its "Still open"
section was not, and an archived report is not somewhere a triage pass looks.
Nothing here was re-verified on the day of the split beyond confirming both
mechanisms still exist and still do not reference each other.

## Summary

Two independent mechanisms decide what the JIT's translation unit has already
declared, and nothing keeps them in step:

- `mangle.c`'s `libc_names` guard -- regenerated from "the headers the
  generated TU includes": stdio, stdlib, string, time, unistd, fcntl, errno,
  setjmp, pthread, ucontext, sys/select, sys/socket, netinet/in, arpa/inet.
  `math.h` is correctly absent, because the emitted TU does not include it.
- `src/jit_win_prelude.h` -- 106 entry points the Windows JIT path prepends.

On that path the TU's effective declaration set is the union. A name added to
`jit_win_prelude.h` tomorrow re-opens the collision the parent report
documented, and re-opens it **silently**: the user's `defn` is called through
the prelude's signature and returns a wrong value, with no diagnostic.

`jit_prelude_win_shadowed` (`src/jit_engine.c:652`) closes the *class* -- it
suppresses the prelude copy for any name the TU itself defines -- so this is
not urgent. What is missing is anything that notices the two lists disagreeing.

## Why it was not just fixed

The parent report considered adding the prelude's math names to `libc_names`
and declined, for a reason that still holds: it changes **shared codegen** for
a **Windows-only** defect, and the gcc `-Wbuiltin-declaration-mismatch` warning
it would silence on the cc path is cosmetic -- gcc compiles that case correctly
today.

## Fix direction

Prefer the build-time assertion over widening `libc_names`:

Derive the prelude's declared names from `src/jit_win_prelude.h` and assert at
build time that they are a subset of `libc_names`. That makes the drift a build
error at the moment it is introduced, on every platform, without changing what
any target emits. It is also verifiable off Windows, which the defect itself is
not -- the whole point is to catch the next addition on the machine that makes
it, not on the one machine that can observe the wrong answer.

If instead `libc_names` is widened, the file's own comment invites it
("over-matching is harmless ... prefer adding a name to leaving one out") -- but
note that route is unverifiable here and changes shared codegen.

## Acceptance

- A name present in `src/jit_win_prelude.h` and absent from `libc_names` fails
  the build with a message naming it.
- The check runs on Linux and macOS, not only on the Windows leg.
