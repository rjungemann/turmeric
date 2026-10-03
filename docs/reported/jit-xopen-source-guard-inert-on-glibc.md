# The emitted unit's `_XOPEN_SOURCE 700` guard is inert on glibc, and it stranded a release

**Severity: high -- it cost the v0.60.0 release.** The `linux-aarch64` leg of
[release run 37110162147](https://github.com/turmeric-lang/turmeric/actions/runs/37110162147)
failed at *Run a program through the JIT from the extracted archive*, so
`Create Release` was skipped by design and `v0.60.0` is a tag with no release,
no assets and no attestation. Product impact on its own is narrower: `tur jit`
on `linux-aarch64` always falls back to the cc path with `TUR-W0070`, which
still prints the right answer. The **cc path is unaffected** -- gcc parses the
offending header fine -- and the archive's *Compile a program from the extracted
archive* step passed.

Found 2026-10-03 cutting v0.60.0. Not a regression in this release: the step
that catches it was added by `923b803ae`, the same commit that made `TUR_JIT`
default ON, so v0.60.0's aarch64 archive is the first to ship the engine on that
platform. `release.yml`'s own comment had named the blind spot --
"linux-aarch64 is a leg no CI job runs the engine on."

## Repro

Needs an aarch64 Linux host (an `ubuntu-24.04-arm` runner; glibc 2.39 here).
On one, from an extracted release archive or a build tree:

```sh
printf '(defn main [] : int (println "hi") 0)\n' > hello.tur
TUR_JIT_DUMP_C=jit.c ./build/tur jit hello.tur
# /usr/include/aarch64-linux-gnu/sys/user.h:30:1: syntax error on struct (expected '<declarator>')
# tur: warning: TUR-W0071: split-runtime path failed to compile; retrying with the full preamble
# /usr/include/aarch64-linux-gnu/sys/user.h:30:1: syntax error on struct (expected '<declarator>')
# tur: warning: TUR-W0070: jit engine could not compile this program; falling back to the cc path
# hi
```

Both arms of the retry fail at the same header line, so the preamble split is
not implicated. The build-tree and extracted-archive dumps are byte-identical
(256,767 bytes), so the archive adds nothing either.

The chain needs no turmeric at all:

```sh
grep -rln 'sys/user\.h' /usr/include
# /usr/include/aarch64-linux-gnu/sys/procfs.h        <- the only one

printf '#include <ucontext.h>\n' | gcc -H -fsyntax-only -xc - 2>&1 | grep user.h
# ... sys/user.h                                      <- reached

printf '#define _XOPEN_SOURCE 700\n#include <ucontext.h>\n' |
  gcc -H -fsyntax-only -xc - 2>&1 | grep user.h
# (nothing)                                           <- NOT reached
```

Of the emitted unit's 19 includes, `<ucontext.h>` is the only one that reaches
`sys/user.h`:

```
<ucontext.h>
 +- aarch64-linux-gnu/sys/ucontext.h
     +- aarch64-linux-gnu/sys/procfs.h
         +- ... -> sys/user.h
```

## Root cause

`src/compiler/emit_module.c:13251` emits `#define _XOPEN_SOURCE 700`
immediately before `<ucontext.h>` -- but `:13237`-`:13240` have already emitted
`<sys/select.h>`, `<sys/socket.h>`, `<netinet/in.h>` and `<arpa/inet.h>`. In the
emitted text those land at lines 53-56 and the `#define` at line 63. By then
glibc's `features.h` has been processed, and it is include-guarded, so the macro
is never re-evaluated: it has no effect on anything.

`sys/ucontext.h` is therefore read with `__USE_MISC` live, which is what makes
it include `sys/procfs.h`. That reaches `sys/user.h`, whose
`struct user_fpsimd_struct` declares `__uint128_t vregs[32]` at the line c2mir
names.

The ordering is deliberate and load-bearing on macOS, which is why the obvious
fix is wrong:

- `:13231` (**T24**) -- the BSD networking headers MUST be processed *without*
  `_XOPEN_SOURCE`, or macOS include guards lock out `INADDR_*`, `sockaddr_in`
  and friends.
- `:13247` (**T21**) -- `<ucontext.h>` MUST precede `setjmp.h`/`pthread.h`, or
  macOS locks in a 56-byte `ucontext_t` where `FiberBlock` needs the full
  880-byte layout.

So **do not simply hoist the `#define` to the top of the unit.** It would
satisfy glibc and break both macOS constraints.

`linux-x86_64` passes the same step. The likely reason is that x86-64's
`sys/user.h` declares its register structs with plain integer types rather than
`__uint128_t` -- *not verified*, and worth checking before relying on it.

## What is NOT established

That `__uint128_t` is specifically why c2mir fails on that header. It is the
only unusual token in the struct at the named line, and c2mir does reject the
token elsewhere ([c2mir-rejects-uint128](c2mir-rejects-uint128.md)), but the
probe arm that would have isolated it was confounded: the `sys/user.h:30` error
is present in *every* `tur jit` run on this platform, including a plain hello,
so "including the header reproduced it" proved nothing. It does not change the
fix -- if `sys/procfs.h` never arrives, neither does `sys/user.h`.

## Fix directions

**Option B -- keep `sys/procfs.h` out of the unit -- is measured DEAD.** Tried in
[probe run 37111947470](https://github.com/turmeric-lang/turmeric/actions/runs/37111947470)
by predefining `_SYS_PROCFS_H` ahead of `<ucontext.h>`. `sys/ucontext.h` does not
merely *include* procfs.h, it **uses** it:

```c
/usr/include/aarch64-linux-gnu/sys/ucontext.h
39: typedef elf_greg_t greg_t;
42: typedef elf_gregset_t gregset_t;
45: typedef elf_fpregset_t fpregset_t;
```

so the stub turns into `error: unknown type name 'elf_greg_t'` and
`<ucontext.h>` stops compiling at all. The same patch in the emitter broke **the
cc path as well** (`tur: cc invocation failed (status 256)`) -- strictly worse
than the bug, since the cc path is what works today. Do not retry this shape.

Worth noting why the narrower variant ("stub procfs, but supply the three
`elf_*` typedefs") is also unpromising: on aarch64 glibc `elf_fpregset_t` is
`struct user_fpsimd_struct` -- the very struct carrying `__uint128_t`. If that
holds, `<ucontext.h>` on this platform **cannot** be included without a
frontend that understands the type. *Unverified* -- read
`/usr/include/aarch64-linux-gnu/sys/procfs.h` on an arm box before relying on
it.

That leaves:

1. **Teach c2mir `__uint128_t`** in the vendored fork -- now the primary route.
   It fixes this *and* user inline C on arm64, and it disturbs none of the
   carefully-ordered include dance. See
   [c2mir-rejects-uint128](c2mir-rejects-uint128.md),
   `external/mir/VENDORED.md`, and the layout prior art in
   `docs/archive/history/jit-arm64-uint128-align-struct-layout-skew.md`.
2. **Emit `<ucontext.h>` only when the program needs `FiberBlock`.** The unit
   includes it unconditionally; a program that uses no fibers would then never
   reach the header, and `hello.tur` is such a program. *Untested*, and it
   narrows rather than closes the gap -- any fiber-using program on aarch64
   still falls back -- but it is the only route that does not wait on a fork
   change. Check what else in the preamble references `ucontext_t`
   unconditionally before costing this.
3. **Fix the inert `_XOPEN_SOURCE` properly** -- make it effective on glibc
   without violating T24/T21. Nothing cheap suggests itself: the macro has to
   be live before `features.h` is first processed, and T24 requires the
   networking headers to be processed *before* it is. A separate translation
   unit for the fiber code is the shape that could satisfy both, which is a much
   larger change than this bug justifies on its own.
4. Relaxing the aarch64 JIT check is **not** a fix: it would publish an archive
   whose shipped engine cannot run, which is exactly what the step was added to
   prevent.

Whichever lands, `v0.60.0`'s tag has to move onto it (or the fix ships as
`v0.60.1`) -- the tag as pushed has no release behind it.
