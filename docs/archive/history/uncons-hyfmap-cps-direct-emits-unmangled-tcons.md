# `_un_uncons_hyfmap` cps->direct body emits the UNMANGLED `tcons` (undefined; survives only via -O2 DCE)

> **Status:** RESOLVED 2026-07-19. `emit_term`'s `CT_LETCALL` (cps->direct) arm in
> `src/compiler/emit_cps_ir.c` now resolves the callee through
> `find_mono_clone_for_call` -- exactly as the sibling `CT_TAILCALL` cps->direct
> arm already did -- so the generated helper emits the DEFINED monomorph
> `tcons__spec__tur_adt_Cons__int___int64_t_int64_t(...)` instead of the unmangled
> generic `tcons`. `find_mono_clone_for_call` returns NULL for any callee with no
> registered ABI specialization, so the change is a no-op for every non-templated
> cps->direct call; the diff is exactly one line in each of the 139 codegen
> snapshots (regenerated in the same commit) and nothing else moves.
>
> Because the clone returns a boxed-ADT pointer (`tur_adt_Cons__int *`) while the
> cps->direct word slot is `int64_t`, the resolved-clone assignment is carried
> through `(int64_t)(intptr_t)` -- otherwise swapping the unmangled generic (whose
> implicit-int declaration masked the mismatch) for the real pointer-returning
> clone would merely trade the `-Wimplicit-function-declaration` warning for a
> `-Wint-conversion` one. With the cast the helper is fully `-Wall` clean:
> `printf '(defn main [] : int 0)\n' | tur emit-c | cc -c -Wall -` no longer warns
> about `tcons`, and the trivial program builds/links/runs. Full suite green
> (2202 passed, 0 failed).

**Severity:** low (latent; masked by dead-code elimination -- the helper is an
unused `static`, so `-O2` drops it before the undefined symbol reaches the
linker).  Present in EVERY emitted program (flag-off baseline), including a
trivial `(defn main [] : int 0)`.

## Repro

```sh
printf '(defn main [] : int 0)\n' > hw.tur
./build/tur emit-c hw.tur > hw.c
cc -c -Wall hw.c -o /dev/null      # warning: implicit declaration of function 'tcons'
```

```c
/* hw.c */
static intptr_t _un_uncons_hyfmap_j0(intptr_t env, intptr_t __t2__slot, DK *__kont) {
    ...
    out_997 = tcons(__t1, __t2); /* cps->direct */     // <- UNMANGLED bare `tcons`
    return dk_run(__kont, (intptr_t)(out_997));
}
```

The only definition emitted is the monomorph
`tcons__spec__tur_adt_Cons__int___int64_t_int64_t(int64_t, int64_t)` -- the bare
`tcons` symbol is never defined.  The call site inside `_un_uncons_hyfmap_j0`
(a generated hylomorphism unfold-fmap helper) uses the unmangled name instead of
the monomorphized one.

## Why it does not break the build

`_un_uncons_hyfmap_j0` is a `static` function that no live code references in
these programs, so at `-O2` it is eliminated before the undefined `tcons`
external reference matters.  Under `-O0` (or if the helper ever became reachable)
this would be a hard link error `undefined reference to 'tcons'`.  The suite
compiles fixtures at `-O2` (`TUR_CC_FLAGS` in tests/run.sh), so it never fails;
the warning is just `-Wall` noise (not `-Werror`).

## Root cause direction

The cps->direct lowering of the `_un_uncons_hyfmap` helper (the generated
hylo/uncons fmap; emitted as part of the always-linked stdlib list preamble)
resolves the `cons`/`tcons` constructor to its unmangled stdlib name rather than
the monomorph clone that the rest of the program uses
(`tcons__spec__tur_adt_Cons__int___int64_t_int64_t`).  Whatever call-resolution
step rewrites `tcons` -> `tcons__spec__...` for ordinary call sites is not applied
inside this generated helper's cps->direct body.  Fix: route the helper's
constructor call through the same monomorph-clone resolution
(`find_mono_clone_for_call` / the emit-time name mangling) so it emits the
defined symbol.

## Why not fixed now

Adding the mangled name (or a forward declaration of `tcons`) to this helper
changes codegen for EVERY snapshot that emits the hylo helper -> a broad fixture
regen.  It is pure hygiene (no runtime effect -- the code is dead), so it is a
coordinated-regen cleanup, not on the endgame track.  Unrelated to the CPS/DK
effect-lowering work.
