# `tur --interpret` segfaults: null `ReaderMacroRegistry` in `cmd_eval_h`

**Severity:** high -- the documented v1 Run target for the desktop editor
spike (`tur --interpret <file>`) is unusable on a minimal hello-world.

**Repro:**

```sh
cat > /tmp/spike-hello.tur <<'EOF'
;;; spike-hello -- minimal Turmeric run target.
(defn main [] : int
  (println "hello from spike")
  0)
EOF
./build/tur --interpret /tmp/spike-hello.tur
```

**Observed:**

```
src/main.c:5345:25: runtime error: member access within null pointer
  of type 'struct ReaderMacroRegistry'
SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior main.c:5345:25
AddressSanitizer:DEADLYSIGNAL
==... ERROR: AddressSanitizer: SEGV on unknown address 0x0...18
  #0 cmd_eval_h  src/main.c:5345
  #1 main        src/main.c
```

**Expected:** prints `hello from spike` and exits 0, the same way
`./build/tur run /tmp/spike-hello.tur` does today.

**Workaround:** use `tur run` (AOT) or `tur check` for now -- both work
on the same fixture.

**Root cause (likely):** `cmd_eval_h` at `src/main.c:5345` dereferences
the `ReaderMacroRegistry` global before it has been initialized on the
`--interpret` codepath. The AOT `run` path initializes it earlier in
`main`; the `--interpret` shortcut appears to skip that setup.

**Related work:** Phase 1 of
`docs/upcoming/turmeric-scite-desktop-plan.md` (and the in-progress
Lite XL replacement plan) bind Run to `tur --interpret`. With this bug
open they fall back to `tur run` instead.

---

## Resolution

**Root cause:** the initial guess (skipped initialization on the
`--interpret` codepath) was wrong. The actual fault was an ODR
violation between `src/turi/env.c` and every TU that included
`turi/env.h` after a system header (notably `src/main.c`). On macOS,
the size of `ucontext_t` -- a field embedded in `TuriEnv` as
`sched_ctx` for the async scheduler -- depends on whether
`_XOPEN_SOURCE` is defined before the first system header that pulls
`<sys/_types/_ucontext.h>` in transitively. `env.c` includes only
`env.h` (which defines the macro first) and saw the POSIX-XSI
`ucontext_t` at 880 bytes. `main.c` included `<signal.h>` /
`<sys/wait.h>` etc. before `env.h`, which materialized the legacy
56-byte stub variant. Net result: `sizeof(TuriEnv)` differed by ~832
bytes between TUs, and every field declared after `sched_ctx`
(including `reader_macros`) sat at a different offset in each TU's
view of the struct. `cmd_eval_h` then read `reader_macros` from a
wholly different position than where `turi_env_new` wrote it.

The crash was deterministic only because the misread offset happened
to land inside the calloc'd zero region. Other fields after
`sched_ctx` were silently misread on every interpret-mode run; this
bug had been latent across the whole codebase.

**Fix:** define `_XOPEN_SOURCE=700` *and* `_DARWIN_C_SOURCE`
project-wide via `add_compile_definitions` in `src/CMakeLists.txt` on
APPLE. `_XOPEN_SOURCE` alone would have hidden several macOS-specific
extensions (`mkstemps`, `NSIG`, `st_mtimespec`) that the runtime needs,
so `_DARWIN_C_SOURCE` keeps the BSD surface visible on top of
POSIX-XSI. Every TU now sees the same `ucontext_t` -- and therefore
the same `TuriEnv` layout -- regardless of include order.

**Diagnostic technique:** the diagnosis came from adding paired
`fprintf` probes in `cmd_eval_h` and `turi_env_new` that printed
`offsetof(TuriEnv, reader_macros)`, `sizeof(TuriEnv)`,
`sizeof(ucontext_t)`, and `_XOPEN_SOURCE` defined-ness. The two TUs
reported different sizes, immediately localizing the cause to a
struct-layout disagreement rather than a missing init.

**Verification:** `./build/tur --interpret /tmp/spike-hello.tur` now
prints `hello from spike` and exits 0; the Lite XL plugin's
`turmeric:run-file` flipped back from the `tur run` workaround to
`tur --interpret` (its documented v1 target). The 206 pre-existing
`-Wint-conversion` fixture build failures observed during the gate
suite are orthogonal -- they reproduce without this fix on Apple
clang 17, which promoted that warning to a default error.
