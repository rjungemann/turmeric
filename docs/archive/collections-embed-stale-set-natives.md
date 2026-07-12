# `tur_collections_embed` harness calls `set-add`/`set-member?` as natives that no longer exist

**RESOLVED (2026-07-12):** Fixed the stale harness per the preferred fix
(option a). `tests/turi/collections-embed.c` now `(load "stdlib/set.tur")` on the
first embed env before exercising `set-add`/`set-member?`, matching the second
block and how a real embedder reaches the public Set surface. The pristine raw-
native HAMT block was reordered to run *before* the load, since loading
stdlib/set.tur pulls in the hamt/map modules and re-elaborates those names.
`ASAN_OPTIONS=detect_leaks=0 ./build/tur_collections_embed` -> "All collection
embedding tests passed." (exit 0). No product code touched.


**Severity: LOW (stale test, no product defect; CI red only. The interpreter
change is intentional and correct -- the harness's expectations went stale).**

## Summary

The `tur_collections_embed` CI target (`tests/turi/collections-embed.c`) fails 4
assertions:

```
FAIL [set count]:       error: unknown function or operator 'set-add'
FAIL [set member? hit]: error: unknown function or operator 'set-member?'
FAIL [set member? miss]:error: unknown function or operator 'set-member?'
FAIL [set union count]: error: unknown function or operator 'set-add'
```

All four are in the harness's **first** env block (lines ~120-134), which
evaluates `set-add` / `set-member?` on a *fresh* `turi_env_new()` **without
loading `stdlib/set.tur`**, expecting them to be built-in low-level natives.

## Minimal repro

```sh
ASAN_OPTIONS=detect_leaks=0 ./build/tur_collections_embed   # -> "4 test(s) FAILED.", exit 1
```

Or directly against a fresh embed env (no stdlib load):

```c
turi_eval(env, "(let [s (set-add (set-add (set-new) 10 10) 20 20)] (set-count s))");
/* -> TURI_ERROR "unknown function or operator 'set-add'" */
```

The harness's **second** block (lines ~179+) that does
`(load "stdlib/set.tur")` first still passes -- `set-add`/`set-member?` resolve
there as stdlib defns.

## Root cause

`set-add` / `set-remove` / `set-member?` were intentionally **de-registered as
interpreter natives** and reimplemented as pure-Turmeric defns in
`stdlib/set.tur` (they dispatch `MapKey[A]` and delegate to the content-keyed
`-eq-o` raw natives). The registration site documents this deliberately at
`src/turi/collections_native.c:1114-1121`:

```c
/* set-add / set-remove / set-member? are now pure-Turmeric defns in
 * stdlib/set.tur ... Registering the old plain-tur_hamt_set natives here would
 * shadow those defns and silently drop the comparator ... so they are not
 * registered ... */
```

That change landed in **#648** (`3edc33f`, 2026-07-09). The harness's first-env
block was written in **#641** (`bef1b21`, 2026-07-07), *before* the move, and
was never updated -- so it still calls `set-add`/`set-member?` on an unloaded
env. `set-new` / `set-count` / `set-union` are still natives (hence those parts
of the same block work), which is why only the `set-add`/`set-member?` lines
fail.

## Fix directions

- **Preferred (fix the stale harness):** either (a) have the first-env block
  load `stdlib/set.tur` before using `set-add`/`set-member?` (matching the
  second block, and matching how a real embedder uses the public Set surface),
  or (b) rewrite those 4 first-env assertions in terms of the still-native
  low-level layer (`set-add-eq-o` / `set-has-eq-o?` with an explicit
  box/cmp/owned? triple) if the intent is specifically to exercise the natives
  without any stdlib load. Option (a) is simpler and mirrors the passing block.
- **Do not** re-register `set-add`/`set-member?` as plain natives to satisfy the
  harness -- `collections_native.c:1114` explains that re-registering them
  shadows the stdlib defns and silently drops the comparator (content dedup /
  membership would fall back to pointer identity). The de-registration is
  correct; the test is what is stale.

## Scope

Pre-existing and independent of the emit_cps.c removal work (this branch touched
neither `collections-embed.c` nor `collections_native.c`; the harness has been
stale since #648 on `main`). Surfaced while triaging CI failures on the
CPS-lowering-removal branch.
