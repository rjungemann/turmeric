# Sanitizer (`-fsanitize=address`) builds of `tur` always exit `1` — LeakSanitizer flags the env's intentionally-retained module-key strings on every run

> **RESOLVED.** The module-private qualified key at `src/turi/eval.c` (the
> `EX_FN_DEF` path) is now allocated from the env's `sym_arena` instead of a
> bare `malloc`. Every `EnvBinding->name` is already expected to point into
> `sym_arena`, and `turi_env_free` reclaims that arena wholesale via
> `arena_free(&env->sym_arena)` -- so the key is now genuinely owned by the env
> and released at teardown. Under ASan/LSan a clean interpret/REPL run
> (`:quit` or immediate EOF) now exits `0` with no leak reported, and the full
> suite is green. The "leak ok" assumption in the old comment is now true under
> LSan too.

**Severity:** low (sanitizer-build-only; no correctness impact and no effect on
release builds — the OS reclaims the memory at exit regardless). Two knock-on
costs make it worth fixing: (1) an ASan/LSan `tur` exits **non-zero on every
successful run**, so any test harness that asserts on the process exit code
fails against a sanitizer build; and (2) the recurring "known" leak is noise
that can **mask a real leak** in the same run.

**One-line:** `eval_expr_impl` (`src/turi/eval.c:7632`) `malloc`s the qualified
`"<module>/<name>"` key for a module-private `defn` and hands the pointer to the
environment, which never frees it (`/* env keeps the pointer; leak ok */`).
`turi_env_free` doesn't reclaim these keys, so LeakSanitizer reports a direct
leak at exit and — with its default `exitcode=1` — forces the process to exit
`1` even on a clean run. The stdlib preload defines a couple of private `defn`s,
so **even an empty REPL session (immediate EOF) leaks 68 bytes and exits 1.**

## Repro

Build `tur` with AddressSanitizer (the local dev/debug build here has it: `nm
build/tur | grep -c __asan` → 39). Then any successful run exits non-zero:

```sh
export TUR_STDLIB_DIR="$PWD/stdlib"

printf ':quit\n' | build/tur repl >/dev/null 2>&1 ; echo "exit=$?"   # exit=1
build/tur repl </dev/null          >/dev/null 2>&1 ; echo "exit=$?"   # exit=1 (no input at all)
```

The leak report is stable and minimal — no user input required:

```
==...==ERROR: LeakSanitizer: detected memory leaks

Direct leak of 68 byte(s) in 2 object(s) allocated from:
    #0 ... malloc
    #1 ... eval_expr_impl /home/.../src/turi/eval.c:7632

SUMMARY: AddressSanitizer: 68 byte(s) leaked in 2 allocation(s).
```

Confirming it is *only* the leak forcing the exit code (behavior is otherwise
correct — eval works, `:quit`/EOF exit the loop cleanly):

```sh
printf ':quit\n' | ASAN_OPTIONS=detect_leaks=0 build/tur repl >/dev/null 2>&1 ; echo "exit=$?"  # exit=0
printf ':quit\n' | ASAN_OPTIONS=exitcode=0     build/tur repl >/dev/null 2>&1 ; echo "exit=$?"  # exit=0
```

A non-sanitizer (release) build exits `0` normally — the allocation still isn't
freed, but there's no LSan pass to notice and the OS reclaims it at exit.

## Root cause

`src/turi/eval.c:7628-7634`, in the top-level `defn` path of `eval_expr_impl`:

```c
const char *qkey = NULL;
bool is_private = (modname && !exported);
if (is_private) {
    size_t need = strlen(modname) + 1 + strlen(fname) + 1;
    char *qk = (char *)malloc(need);  /* env keeps the pointer; leak ok */
    if (qk) { snprintf(qk, need, "%s/%s", modname, fname); qkey = qk; }
}
```

The qualified key for a module-private `defn` is `malloc`'d and stored into the
environment (as the binding key). The "leak ok" assumption is that the string
lives for the whole process, so nobody frees it. That holds for a plain build,
but it means the string is **owned by the env yet not released in
`turi_env_free`** — so `turi_repl_run` (which does call `turi_env_free(env)`
before `return 0`, `src/turi/repl.c:1385-1386`) still leaves these keys
unreachable-but-unfreed, and LSan reports them.

`tur_repl_run` itself is not the culprit: the loop `break`s on `:quit`
(`repl.c` ~line 1110) and on EOF, then returns `0`; `cmd_repl` (`main.c:5890`)
and `main` propagate that `0`. The non-zero exit is entirely LSan's
`exitcode` on the reported leak.

The two leaked objects come from the REPL/interpreter **stdlib preload**
defining private `defn`s at startup — hence the count is fixed at 2 regardless
of user input. The same site will leak one key per module-private `defn`
elaborated in any interpret/REPL session.

## Fix direction

The clean fix is to make the env **own** these keys and free them when the
binding table is torn down:

- Give `turi_env`'s key storage a "this key is heap-owned" bit (or intern
  qualified keys into a per-env arena/pool), and free them in `turi_env_free`.
  Then the "leak ok" comment at `eval.c:7632` becomes true under LSan too.
- Alternatively, if these keys are meant to outlive individual envs, intern them
  in a process-wide table that is registered with LSan as a root
  (`__lsan_ignore_object` / `LSAN_OPTIONS` suppression), so intentional
  retention is explicit rather than reported as a leak.

Either way the goal is: **a clean interpret/REPL run under ASan should exit
`0`**, so the sanitizer build is usable in exit-code-sensitive test harnesses
and real leaks aren't buried under this known one.

Harness-side workaround until then: run the sanitizer `tur` with
`ASAN_OPTIONS=detect_leaks=0` (or `exitcode=0`), or test against a non-sanitizer
build.

## How this surfaced

Discovered while bringing up the **Trowel** editor's Linux build. Trowel's smoke
suite drives a real `tur` through the REPL and asserts on shutdown; its
`tests/smoke/test_shutdown.py::test_repl_quit_exits` (asserts REPL process exit
code `== 0`) was the one failing case when run against a locally-built,
ASan-instrumented `tur` on Linux. All other REPL smoke tests pass. Root-causing
that single failure led here: it is not a Trowel bug and not a REPL logic bug —
it's this LSan-reported retained allocation forcing a non-zero exit in the
sanitizer build.
