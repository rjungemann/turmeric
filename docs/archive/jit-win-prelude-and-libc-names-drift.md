# The Windows JIT prelude and `mangle.c`'s `libc_names` can drift apart silently

**RESOLVED 2026-09-17** -- see Resolution at the end. Both acceptance criteria
are met; the check is stricter than the report asked for in one respect and
weaker in another, both recorded there.

**Severity:** low today, latent -- and the failure mode is a **wrong answer, not
a build error**, which is what earns it a tracker rather than a comment.

**Status:** open. Split out 2026-09-10 from
[jit-win-prelude-shadows-user-fn](jit-win-prelude-shadows-user-fn.md)
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

## Resolution (2026-09-17)

Taken as the report preferred -- derive the prelude's names and assert, rather
than widen `libc_names` -- as **property D** of the existing
`tests/check-libc-collision-list.sh`, which is already the static guard for
this exact table (`tur_libc_collision_list` in CMakeLists.txt). It is a pure
source read with no built compiler and no Windows, so it runs wherever ctest
runs.

One deviation from the letter of the acceptance criteria, stated plainly: this
repo has no build-time script hooks -- every source-text precondition of this
kind (`check-cc-warn-ratchet`, `check-span-coverage`, `check-typekind-mangle-exhaustive`,
~25 others) is a ctest target -- so "fails the build" is realised as "fails
`tur_libc_collision_list`". The property the acceptance was after is intact:
the failure fires on every platform, at the moment the edit is made, naming the
symbol.

### The extractor is deliberately stricter than `jit_engine.c`'s

`jit_prelude_decl_name` (`src/jit_engine.c:614`), which drives the run-time
`jit_prelude_win_shadowed` mitigation, only recognises SINGLE-LINE declarations
-- its own comment notes it skips "the two multi-line declarations". So `qsort`,
`pthread_create`, `pthread_mutex_init` and `pthread_key_create` have no run-time
shadow protection at all, which makes it *more* important that they sit in
`libc_names`, not less. Mirroring that scanner's blind spot into the check would
have exempted precisely the riskiest names, so the check joins declarations
across lines first. Verified: a synthetic multi-line prelude declaration fails
the check, where the `jit_engine.c` scanner would not see it.

### The 25 acknowledged gaps

The check found 108 prelude declarations, 83 already in `libc_names` and 25 not:
twenty `math.h` names (`sqrt`, `pow`, `log2`, ...) and five Win32/UCRT spellings
(`Sleep`, `__acrt_iob_func`, `_errno`, `_fileno`, `_setmode`). These are listed
in a `PRELUDE_ONLY` set in the script rather than added to `libc_names`, for the
reason under "Why it was not just fixed" above, which still holds: adding them
changes SHARED codegen (every platform's mangling of a user `defn` by one of
those names) to close a Windows-only exposure.

That makes `PRELUDE_ONLY` a ratchet, not an exemption, and it is checked in both
directions so it can only shrink quietly:

| Drift shape | Result |
| --- | --- |
| New single-line prelude decl absent from `libc_names` | FAIL, names it |
| New **multi-line** prelude decl absent from `libc_names` | FAIL, names it |
| `PRELUDE_ONLY` entry the prelude no longer declares | FAIL ("delete them") |
| `PRELUDE_ONLY` entry since added to `libc_names` | FAIL ("the exemption is spent") |

All four were exercised by hand against the real tree (the prelude header and
the script were restored afterwards; `git diff` confirms only the script and the
CMakeLists comment changed). Retiring an entry is now a two-line edit whose
correctness the check itself enforces.

### What is still not covered

The check reads declarations, not definitions: `__tur_mirc_fd_isset` is a
file-static helper in the prelude and is skipped, as are the typedefs and the
`#define`d macros (`stdin`, `errno`, `FD_ZERO`, ...). A collision through a
prelude MACRO rather than a declaration would not be caught. Nothing in the
parent report's failure mode goes through one, and a macro collision is a build
error rather than a wrong answer, so it is left out on purpose rather than
missed.
