# `match` arm error leaks the linear-state snapshot buffers

> **Resolved.** `elab_match` (`src/compiler/elab_structs.c`) now routes every
> failing exit through a single `match_fail:` epilogue that frees `covered`, the
> linear snapshot buffers (`match_lin_bindings` / `match_lin_before`), and the
> per-arm arrays (`arm_lin_states` rows + array, `arm_diverges`). The 22
> `free(covered); return NULL;` bail-outs became `goto match_fail;`, and the
> merge/else blocks NULL out the linear buffers after freeing so the epilogue's
> NULL-safe frees never double-free. Verified: `ASAN_OPTIONS=detect_leaks=1
> ./build/tur check leak.tur` is now leak-clean, and `bash tests/run.sh` passes
> (1925/1925).


**Severity:** low. Error path only -- it fires when a `match` fails to elaborate
(bad constructor, malformed arm, ...) and the process is about to exit with a
diagnostic anyway. The successful compile / codegen path is leak-clean, so
`bash tests/run.sh` (which leak-checks only successful builds) does not catch it.
Worth fixing so an ASan/LSan run over a *failing* compile is clean.

## Minimal repro

```turmeric
(defdata Color (Red) (Green) (Blue))

(defn main [] : int
  (match (Red)
    (Purple) 0        ; not a constructor of Color -> hard error
    (Red)    1
    (Green)  2
    (Blue)   3))
```

```sh
ASAN_OPTIONS=detect_leaks=1 ./build/tur check leak.tur
```

```
leak.tur:5:6: error: match: 'Purple' is not a constructor of 'Color'
==...==ERROR: LeakSanitizer: detected memory leaks
    #1 ... in linear_state_snapshot_bindings src/compiler/elab_core.c:1095
    #1 ... in linear_state_snapshot_bindings src/compiler/elab_core.c:1096
SUMMARY: AddressSanitizer: 144 byte(s) leaked in 2 allocation(s).
```

## Root cause

`elab_match` (`src/compiler/elab_structs.c`) snapshots the linear-binding state
before walking the arms:

```c
// src/compiler/elab_structs.c:3198
n_match_lin = linear_state_snapshot_bindings(e->scope, &match_lin_bindings,
                                             &match_lin_before);
if (n_match_lin > 0) {
    arm_lin_states = (bool **)calloc(n_arms, sizeof(bool *));   // :3201
    arm_diverges   = (bool  *)calloc(n_arms, sizeof(bool));     // :3202
}
```

`linear_state_snapshot_bindings` (`src/compiler/elab_core.c:1090`) *always*
`malloc`s its two buffers (cap = 16), regardless of how many linear bindings are
in scope, and hands ownership to the caller. These buffers (plus `arm_lin_states`
/ `arm_diverges`) are only freed at the normal end of `elab_match`
(`elab_structs.c:3796-3808`).

Every error-return inside the arm loop bails with `free(covered); return NULL;`
and frees `covered` **only** -- leaking `match_lin_bindings`, `match_lin_before`,
and (when allocated) `arm_lin_states` / `arm_diverges`. The `'Purple' is not a
constructor` path at `elab_structs.c:3293` is one such return; the sibling
error-returns at (at least) `3308, 3315, 3322, 3335, 3348, 3354, 3367, 3418,
3599, 3609, 3672, 3704, 3741, 3760` have the same shape. The 144 bytes = the two
16-slot buffers (16*8 + 16*1, rounded by the allocator).

## Fix directions

A single cleanup epilogue reached by every exit is the clean fix:

- Replace the scattered `free(covered); return NULL;` bail-outs in the arm loop
  with `goto match_fail;`, and add a `match_fail:` label near the existing
  `3796-3808` cleanup that frees `covered`, `match_lin_bindings`,
  `match_lin_before`, and the per-arm `arm_lin_states` (each row) /
  `arm_diverges` before `return NULL`. The success path can share the same
  epilogue with a status flag.
- Lower-effort alternative: free the four snapshot buffers at each error-return
  alongside `covered` (mirrors what `elab_forms.c`'s `if`/`when` paths already do
  -- they free `move_bindings` / `before_states` / `lin_bindings` on every
  branch). More error-prone as arms are added; prefer the single epilogue.

Note `arm_lin_states` is an array of per-arm `bool*` rows (`elab_structs.c` fills
`arm_lin_states[ai]`), so the epilogue must free each non-NULL row before the
outer array.
