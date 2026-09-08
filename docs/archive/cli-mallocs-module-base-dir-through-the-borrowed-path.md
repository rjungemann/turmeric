---
title: "Two CLI sites `malloc` `module_base_dir` and store it through the borrowed-pointer path, so it is never freed"
category: Reported
description: "src/main.c:7827 and :8578 malloc a directory string and assign env->module_base_dir directly, leaving module_base_dir_owned false -- so turi_env_free never frees it. turi_env_set_module_base_dir exists for exactly this and strdups + owns. Small and once-per-process, but it fires on every --interpret run under ASAN_OPTIONS=detect_leaks=1, which is the channel used to chase real interpreter leaks."
---

# The CLI stores a malloc'd `module_base_dir` as if it were borrowed

**RESOLVED 2026-09-08.** Direction 1: both sites now call
`turi_env_set_module_base_dir` with a temporary slice and free it immediately,
so the env owns its own strdup and `turi_env_free` releases it. A clean
`--interpret` run is now leak-free with detection ON (it previously reported
the directory-path allocation every time). The `*args*` cons cells still
report when arguments are passed -- that is the sibling allocation this report
explicitly excluded, unchanged and still process-lifetime by design.

Verified beyond the leak: `module_base_dir` is what makes `(import ...)`
resolve relative to the script, so the import-bearing ctest targets
(`tur_eval_import`, `tur_offtree_load`, `tur_saffron_import`,
`tur_repl_spice_load`, `tur_repl_spice_reload`) were run and pass, alongside
run.sh 2891/0 and run-turi.sh 1982/0.

**Severity: low.** A few dozen bytes, once per process, in a CLI that is about
to exit. What makes it worth a row rather than nothing: it is an ownership-
contract violation with a one-line fix, and it is **noise in a diagnostic
channel**. `CLAUDE.md` documents `ASAN_OPTIONS=detect_leaks=1 bash
tests/<harness>.sh` as the way to opt back into leak detection on the
interpreter harnesses; these two fire on every such run and sit at the top of
the report, where a real finding would be.

Found while hand-checking a D6 fixture under `--interpret` with leak detection
left on.

## Repro

```
$ ./build/tur --interpret any-saffron-file.tur
...
==15352==ERROR: LeakSanitizer: detected memory leaks

Direct leak of 43 byte(s) in 1 object(s) allocated from:
    #0 ... in malloc
    #1 ... in cmd_eval_h /home/user/turmeric/src/main.c:7824
```

43 bytes is the length of the script's directory path -- it scales with where
the file lives, which is the tell.

## Root cause

`TuriEnv` has an explicit ownership protocol for this field:

```c
/* Gap 4: true when module_base_dir was set via turi_env_set_module_base_dir
 * (heap-owned, freed by turi_env_free).  Direct field assignments by the CLI
 * leave this false and retain their existing borrowed-pointer semantics. */
bool module_base_dir_owned;
```

and a setter that honours it (`src/turi/env.c`):

```c
void turi_env_set_module_base_dir(TuriEnv *env, const char *path) {
    ...
    char *copy = strdup(path);
    env->module_base_dir       = copy;
    env->module_base_dir_owned = true;
}
```

`turi_env_free` then frees it when `module_base_dir_owned` is set.

Both CLI sites bypass the setter -- and each one **mallocs first**:

```c
/* src/main.c:7824 (cmd_eval_h) and :8575, identical shape */
char *dpath = (char *)malloc(dlen + 1);
memcpy(dpath, path, dlen);
dpath[dlen] = '\0';
env->module_base_dir = dpath;        /* <- borrowed path, owned stays false */
```

The field's contract is not violated by direct assignment as such -- a genuinely
borrowed pointer (into `argv`, say) is exactly what that path is for. It is
violated by assigning a **fresh allocation** through it: the pointer is then
owned by nobody and `turi_env_free` correctly declines to free what it was told
it does not own.

Note the sites also need a `strrchr` slice rather than the whole string, which
is presumably why they hand-rolled the copy instead of calling the setter.

## Fix directions

1. **Call the setter with a NUL-terminated slice.** It strdups, so the caller's
   buffer can be a stack array (`char dir[PATH_MAX]`) or a temporary freed
   immediately after -- no malloc that outlives the call. Removes both leaks and
   makes the two sites obey the protocol the field documents.
2. **Keep the malloc and set `module_base_dir_owned = true` alongside it.**
   Fewer lines, and it hard-codes knowledge of the ownership flag into `main.c`
   -- exactly what the setter exists to avoid. Do not prefer this.

Direction 1. Both sites in the same change; they are the same four lines twice.

## Not this bug

The `*args*` cons cells allocated a few lines above (`src/main.c:7809`) also
show up under `detect_leaks=1`, and are **not** this defect: they are the
process-lifetime interpreter allocations `CLAUDE.md` describes ("the
tree-walking turi/eval interpreter intentionally never frees its
closures/registered natives"), which is why the harnesses default to
`detect_leaks=0`. They would want a separate decision, not this fix.

The compiler/codegen path is unaffected -- it is leak-clean and `tests/run.sh`
runs with leak detection ON.
