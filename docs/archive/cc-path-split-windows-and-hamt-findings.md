# cc-path split: the two defects that kept it red, and what they cost to find

**Resolved 2026-09-07.** Both are fixed and the split suite runs clean on Linux
and Windows (`2828 passed, 0 failed`). This is the paper trail from
`tests/split-known-failures-{linux,windows}.txt`, kept after those files were
deleted, because the seams are still live and the method notes generalise.

Context: `--runtime=split` / `TUR_RUNTIME=split` replaces the emitted TU's fixed
runtime preamble with a committed declarations region and links the runtime once
from `libturt_preamble.a`, instead of recompiling it into every program. See
`docs/upcoming/cc-path-preamble-split-plan.md`.

---

## 1. `gc-registry-growth` -- a stack overflow wearing a GC bug's clothes

**Symptom.** No output, exit 127, under split on Windows; the DEFAULT runtime
passed the same fixture with the same compiler and the same build.

**The 127 was a red herring for hours.** It is bash's translation. The real
status is `0xC00000FD` == `STATUS_STACK_OVERFLOW` -- PowerShell reports
`LASTEXITCODE=-1073741571`. One PowerShell invocation collapsed the
investigation; nobody had run one.

**Mechanism.** The monolithic preamble emits `tur_frame` *and its accessors*
into the same TU as the program, as `static inline`
([emit_module.c:10092-10103](../../src/compiler/emit_module.c)). The split
replaced that region with the decls header, where they were bare extern
prototypes with bodies in the archive. Inlined, GCC sees a frame holding one
known thunk and eliminates the 536-byte struct. Behind an opaque call
`&__frame_N` escapes, so every byte must be materialised:

```
build_hychain prologue -- same compiler, byte-identical program C
  default   sub $0x48,%rsp
  split     sub $0x268,%rsp
```

20000 frames x 640 bytes = 12.2 MiB against a **2 MiB** Windows stack reserve
(identical `SizeOfStackReserve` in both PE headers, so not a link-flag
difference). Predicted overflow depth 2097152/640 = 3276; measured, split
printed at 3200 and died at 3300. Relinking the same split build with
`-Wl,--stack,33554432` made it print `20000`.

**Fix.** `INLINE_INTO_DECLS` in `tools/gen-runtime-split.py`: named helpers emit
their bodies into the decls half as `static inline`, with the archive copy kept.
Two things the obvious version got wrong:

- **It is three helpers, not two.** Inlining `tur_frame_init` and
  `tur_frame_push_defer` alone changed *nothing* -- the prologue stayed at
  `0x268` -- because the emitter also writes `tur_frame_fire_lifo(&__frame_N)`
  at scope exit, and one surviving opaque callee is enough for the address to
  escape. It is plain `static` in the preamble, not `static inline`
  (emit_module.c:10110), so it does not appear when grepping for the inline
  ones. **Measuring after the first change, instead of assuming it worked, is
  what caught this.**
- **Archive copies are kept, not omitted.** Omitting works for the first two
  (nothing in the archive calls them) but not for `fire_lifo`, which
  `tur_frame_fire_chain` calls from inside the archive. `static inline` has
  internal linkage, so keeping both conflicts with nothing.

**A wrong elimination that cost real time.** The known-failures file recorded
"NOT recursion depth -- a hand-written 20000-deep recursion runs fine under
split". That probe had no `defer` in its body, so it never allocated a
`tur_frame`. It eliminated depth *alone*; the actual variable is
depth x defer-bearing scope. **A negative result is only as good as the thing it
varied**, and this one sat committed, steering readers away from the cause.

---

## 2. `hamt-lowering-basic` -- "irreducibly lossy" was neither

**Symptom.** A screenful of `implicit declaration of function 'tur_hamt_*'`
under split, on both platforms.

**The conflict is real** and was confirmed directly rather than assumed:

```
error: conflicting types for 'tur_hamt_new'; have 'void *()'
note: previous declaration ... with type 'Hamt *(void)'
```

The emitter writes EITHER `#include "hamt.h"` (a program the compiler lowers
straight onto the hamt API) OR loose `extern void *tur_hamt_new();`
declarations from `stdlib/hamt.tur`'s extern-c -- never both, because gcc
rejects the pair. The committed decls region is generated from ONE canonical
emission, so whichever shape that program used was frozen for everybody:
carrying the include unconditionally broke every loose-extern program, and
guarding it out broke every program needing the header. Both were true, which is
what made it look like a trade-off with no good side. It was filed as "the one
place the split is genuinely LOSSY", with two proposed fixes -- carry both
shapes, or reconcile stdlib's declarations with the header. Both substantial.

**Neither was needed.** Nothing was lost; it was *discarded*. The replaced
preamble region contained that program's own `#include "hamt.h"`, and the swap
threw it away with everything else in the region. `jit_try_split_preamble`
(`src/main.c`) now re-emits the quoted includes it finds there, so each program
gets exactly what it emitted -- the monolithic behaviour, restored.

Quoted includes only: `<...>` system headers stay the decls region's business,
and re-emitting those on Windows would hand the JIT the MinGW SDK headers it
cannot digest ([jit-windows-support-spike](../reported/jit-windows-support-spike.md)).

---

## 3. "Fails on Windows" was never one claim

For most of this branch's life, "the three split failures" was quoted as a
property of the split. It was a property of one configuration. Three
configurations gave three different answers:

| | failures under split |
| --- | --- |
| Linux CI | `hamt-lowering-basic` |
| Windows CI | `hamt-lowering-basic`, `gc-registry-growth` |
| one local Windows box | those two, plus `sx8a-tur-smt-div-mod` |

`sx8a-tur-smt-div-mod` passes on the Windows runner and fails locally --
unexplained, leading hypothesis a toolchain version gap (local gcc 16.1 vs the
runner's MSYS2). It was caught by the CI leg's "every listed failure still
fails" check on that check's first run. Worth noting **which half caught it**:
the subset check ("no unexpected failures") passed on all three shards. A job
that only verified failures were known would have gone green while the baseline
listed a fixture that had stopped failing.

---

## Still open

1. **The DEFAULT runtime is near a stack cliff on Windows.** `gc-registry-growth`
   passes at depth 21000 and overflows at 21800 (2097152/96 = 21845), so at its
   committed depth of 20000 it uses ~1.83 MiB of 2 MiB -- about **9% headroom**.
   Any codegen change adding a few bytes to that frame breaks the fixture in the
   default runtime, presenting as the same content-free "exit 127". Nothing in
   `src/` or `tests/run.sh` passes `-Wl,--stack`, so every `tur`-built Windows
   binary gets 2 MiB where Linux gets 8 MiB. Aligning them would remove the
   cliff.
2. **Shrink `tur_frame`.** 536 bytes per lexical scope is three fixed 32-entry
   arrays, sized for a common case of 1-2 defers. That would make the inlining
   in fix 1 far less load-bearing.
3. **Why `gc-registry-growth` ever passed under split on Linux is unexplained**,
   and the arithmetic says it should not have: at 20000 frames an 8 MiB stack
   allows 419 bytes, less than `sizeof(tur_frame)` alone. Either the Linux frame
   is smaller than can be accounted for, or the split declined the swap there.
   One check settles it: build with `--runtime=split`, `objdump -d` the
   `build_hychain` prologue and read `ulimit -s`. A ~0x230 `sub` means the same
   latent cliff exists on Linux; a ~0x30 `sub` means the split declined and that
   green was vacuous for this fixture.
