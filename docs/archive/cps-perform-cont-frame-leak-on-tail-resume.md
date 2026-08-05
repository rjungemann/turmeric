# Straight-line perform-continuation frame leaks when the handler tail-resumes

**STATUS: RESOLVED (2026-07-19, commit reaping the straight-line perform-cont
frame via `__dk_reap_node`).** Both LH_PERFORM_CONT emit sites in `emit_perform`
now hand the `dk_frame` node to `__dk_reap_node` inlined into `dk_perform` and drop
the post-`dk_perform` `dk_free_node`, so the frame is reclaimed at the outermost
entry boundary on BOTH the normal-return and tail-resume-yield paths. ASan-clean
on all five fixtures below; outputs unchanged; suite 2203/0 flag-off byte-identical.
The fix direction proposed at the bottom of this report is exactly what landed.

**Severity:** low (experimental `--enable=cps-tramp-resume` path only; correctness
is fine -- one 104-byte DK node leaks per suspended perform-cont frame whose
handler tail-resumes). Pre-existing and widespread; NOT introduced by the B8 work
that surfaced it. Matters for graduating the flag: a leaky flag-on path is an
obstacle to flipping `cps-tramp-resume` on by default and deleting the fiber
effect runtime.

## Symptom

Under `--enable=cps-tramp-resume`, a program with a `(handle (perform E) (E [] k)
(resume k v))` shape leaks exactly 104 bytes (`sizeof(DK)`, one node) per
suspended straight-line perform continuation. Reproduces on long-shipping flag-on
fixtures: `cps-backend-effect`, `cps-backend-option-effect`,
`cps-backend-struct-effect`, and (now that they DK-lower) `session-effects` /
`session-mp-effects`. Fixtures whose handler does NOT tail-resume are clean
(`effects-async`, `effect-capture-k`).

```sh
CC="cc -fsanitize=address,undefined -g" \
  ./build/tur --enable=cps-tramp-resume build tests/fixtures/cps-backend-effect/input.tur -o /tmp/e
ASAN_OPTIONS=detect_leaks=1 /tmp/e
# => Direct leak of 104 byte(s): dk_new <- dk_frame <- use_hyask__cps
```

## Root cause

The straight-line perform-continuation (`LH_PERFORM_CONT`) is emitted
(`emit_perform`, src/compiler/emit_cps_ir.c) as:

```c
DK *__pfd0 = dk_frame(use_hyask_pf0, 0, __kont);
int64_t __pfr0 = dk_perform(2, 0, __pfd0);
dk_free_node(__pfd0);        // frees the frame node AFTER dk_perform returns
return __pfr0;
```

The `dk_free_node(__pfd0)` assumes `dk_perform` returns normally. But when the
handler is a DEEP tail-resume handler (E7 trampoline, `dk_handler_tail` /
`dk_tail_resume`), `dk_perform` YIELDS the resumed chain to the driver via
`longjmp(*g_dk_driver, ...)` and never returns to `use_hyask__cps` -- so the
`dk_free_node` line is skipped and `__pfd0` leaks.

This is the same class as the already-fixed `cps-resume-frame-node-leak.md` /
`cps-delimited-dk-node-leak.md` (both archived), which reap their spliced nodes
at the entry boundary via `__dk_reap_node`. The straight-line `LH_PERFORM_CONT`
frame in `emit_perform` still uses the post-`dk_perform` `dk_free_node`, which the
tail-resume yield skips.

## Fix direction

Register the perform-cont frame node for a boundary reap instead of relying on the
skipped post-`dk_perform` free -- exactly the discipline the resume-frame and
delimited-node paths already use:

```c
int64_t __pfr0 = dk_perform(2, 0, __dk_reap_node(dk_frame(use_hyask_pf0, 0, __kont)));
return __pfr0;
```

`__dk_reap_node` (kind=0, a bare single-node free) reclaims it at the outermost DK
entry boundary (`__dk_reap_run` at `__dk_entry_depth == 0`), which is reached on
both the normal-return and the tail-resume-yield paths. Drop the `dk_free_node`.
Caveat to verify: under a MULTI-SHOT resume the node may be consumed more than
once; the boundary reap must free it exactly once (the reap list is a set of
pointers, so a single registration is correct -- confirm the node is not also
freed elsewhere on the normal path, to avoid a double free).

Applies to the two `LH_PERFORM_CONT` emit sites in `emit_perform` (the caps and
no-caps branches), both of which currently emit `dk_free_node(__pfd%d)` after
`dk_perform`.
