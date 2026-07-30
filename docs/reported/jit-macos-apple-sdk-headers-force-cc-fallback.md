# `tur jit` on macOS: Apple SDK headers push 13 fixtures to the cc fallback

**Severity: low (performance, not correctness).** Every affected fixture
PASSES -- the engine's step-6 cc fallback catches it. The cost is that those
programs get no JIT at all, on the platform where the JIT has the least
headroom to begin with.

Split out 2026-07-30 from
docs/archive/jit-macos-full-corpus-extension-and-atexit.md, whose other three
findings are resolved. The count has not moved across three macOS runs
(findings 20.2, 32.2).

## Summary

c2mir refuses three Apple SDK headers that emitted programs reach transitively.
Each refusal is the header probing for a compiler identity c2mir does not
advertise, then `#error`-ing or producing an empty conditional:

| Class | Count | Header | Fixtures |
|---|---|---|---|
| `#error TargetConditionals.h: unknown compiler` | 5 | `TargetConditionals.h:398` | `gc-collects-strong-cycle`, `gc-live-cycle-survives`, `hkt-fmap-rc-result-droppable`, `hkt-instance-rc-construct-result`, `weak-breaks-parent-child-cycle` |
| `empty preprocessor expression` | 3 | `mach/port.h:100`, reached via `mach-o/dyld.h` | `image-hooks-tracked`, `image-reload-hook`, `image-roundtrip` |
| `unresolved import: _OSSwapInt16` | 1 | `libkern/_OSByteOrder.h` (`__DARWIN_OS_INLINE`) | `async-echo-server` |

That is the original 9 from the parent report; findings 32.2 measures the
current macOS fallback total at 60 against Linux's 47, i.e. **13** attributable
to the SDK. The extra 4 over the original survey have not been individually
attributed.

### Attribution corrections (2026-07-30)

Two rows above were wrong in the original write-up and are corrected in place;
the counts are unchanged, and a fresh baseline measured **58** fallbacks with
**14** SDK-attributable, so the "13" holds within drift.

- **`mach/port.h` is not reached via `sys/socket.h`.** Measured with
  `clang -H` on the 15.5 SDK: `sys/socket.h` and `netinet/in.h` reach
  `mach/port.h` **zero** times. The actual path is `<mach-o/dyld.h>`, hoisted
  to file scope from `stdlib/image.tur:49-51` (it needs `_NSGetExecutablePath`)
  -- which is exactly why the three fixtures in that row are the `image-*` set
  and nothing else. `malloc/malloc.h` and `mach-o/dyld.h` are the only two
  headers in play that reach it.
- **The `TargetConditionals.h` row is fixture-local, not preamble-borne.** None
  of the preamble's headers (`stdio.h`, `stdlib.h`, `string.h`, `pthread.h`,
  `setjmp.h`, `sys/socket.h`, `ucontext.h`) reach `TargetConditionals.h` at
  all. Those five fixtures reach it through `<malloc/malloc.h>`, included at
  **function scope** by each fixture's own inline-C heap probe (e.g.
  `tests/fixtures/gc-collects-strong-cycle/input.tur:29-40`). Demand-gating the
  emitted preamble therefore does nothing for this row.
- **`OSByteOrder.h:314` `#error Unknown endianess` is not what fires.** On
  arm64 the SDK routes through `libkern/arm/_OSByteOrder.h`, whose
  `_OSSwapInt16` is plain C declared `__DARWIN_OS_INLINE` (= `static
  __inline__`). c2mir parses the declaration and emits no definition, so the
  failure is a **link-stage** `import of undefined item _OSSwapInt16`, not a
  preprocessor `#error`.

The common root cause across all three rows still holds and is confirmed:
c2mir does not advertise `__GNUC__`, so `sys/cdefs.h:80` takes its
`#warning "Unsupported compiler detected"` branch and the SDK falls through to
conditionals a real compiler never visits.

### `_OSSwapInt16` row: RESOLVED 2026-07-30

That row is fixed and no longer contributes. `src/jit_engine.c` now carries
`_OSSwapInt{16,32,64}` shims in `JIT_SHIMS[]` **plus** matching `JIT_PRELUDE`
prototypes. Measured effect on the full JIT corpus:

```
before:  2391 passed, 3 failed, 47 skipped, 58 via cc fallback
after:   2393 passed, 1 failed, 47 skipped, 31 via cc fallback
```

27 fewer fallbacks, zero regressions, and two fixtures that previously failed
(`httpd-h4-keepalive`, `httpd-h6-routing`) now pass. The reach far exceeds the
one fixture this row named because `htons`/`ntohs` route through the same
inlines, which is the entire `httpd-*` family.

**The `JIT_PRELUDE` prototypes are load-bearing.** Registering the shim
addresses alone reproduces findings 11.7 exactly: with only the SDK's `static
__inline__` declaration visible, c2mir falls back to an implicit int-returning
prototype, the call reads the wrong register, and `htons(8080)` yields `0xb8f6`
instead of `0x901f`. That silently corrupts ports and header lengths -- it
presents as a null-deref SEGV in generated code and as
`bind: Can't assign requested address`, both of which look like engine bugs and
are not. Do not shim a Darwin `static inline` without declaring its real
prototype.

A prerequisite landed with it: `stdlib/httpd.tur` declared a dead
`static const signed char b64tab[256] = { [0 ... 255] = -1 }`. The `[0 ... 255]`
range is a GNU designated-initializer extension c2mir cannot parse, and it
blocked the whole `httpd-*` family at the *compile* stage, upstream of the
link error above. It was never referenced -- the comment directly beneath it
already rebuilds the table at runtime as `tab` -- so it was removed.

### Not fixable by shimming: the `__atomic_*` family

The single largest remaining fallback class (14 fixtures: `arc-*`, `atomic-*`,
`cancel-*`, `future-*`, `once-basic`, `promise-linear`, `workstealing-*`) is
`import of undefined item __atomic_add_fetch` and friends. This is
**deliberate** and should stay that way -- see the note at
`src/jit_engine.c:58-62`. Beyond the non-atomicity argument recorded there,
these builtins are type-generic: the corpus calls `__atomic_add_fetch` on both
`int64_t` refcounts (`stdlib/future.tur:116`) and narrower flags, but MIR binds
one name to one signature, so any single shim writes the wrong width somewhere.
The cc fallback is the correct behavior for this class.

Linux is unaffected -- glibc's headers do not gate on compiler identity the
same way, which is the same asymmetry that hid the `__extension__` bug
(parent report, finding 1).

## Why it is worth fixing despite passing

On Apple Silicon **c2mir, not MIR-gen, is 73% of engine cost** (findings 20.4),
so S2 buys 17% there against Linux's 38%. The plan's own J2 note names the next
lever explicitly: "the lever after S2 is the c2mir front end itself -- chiefly
not re-parsing the Apple SDK headers per program." These 13 are the visible end
of that: programs that cannot use the engine at all because a header stops the
parse.

## Fix directions

1. **Predefine what the headers probe for.** c2mir accepts `-D` options
   (`c2mir_options.macro_commands`). Advertising a compiler identity the SDK
   headers recognize -- and an endianness for `OSByteOrder.h` -- should clear
   all three classes without touching the SDK. Cheapest first step; verify each
   class separately, since `TargetConditionals.h` and `OSByteOrder.h` fail for
   different reasons.
2. **Stop reaching the headers at all.** ~~The S2 split already removed the
   fixed runtime preamble from every per-program compile.~~ It did not: S2
   moved the runtime *definitions* out, but `tur_rt_split_decls.h:45-343`
   carries the identical system-header block, so every JIT'd program still
   preprocesses `pthread.h`, `ucontext.h`, the BSD socket set, `regex.h` and
   `time.h`. Removing them is therefore a real project, not a follow-on:
   `jmp_buf`, `ucontext_t`, `pthread_mutex_t` and `pthread_t` are embedded
   **by value** in structs the per-program TU must lay out
   (`tur_rt_split_decls.h:764-765` puts two `ucontext_t` in FiberBlock), so a
   host-resident declaration means hand-writing those ABI layouts per platform
   and arch -- the same hazard class as
   docs/archive/jit-arm64-uint128-align-struct-layout-skew.md.

   Note also that demand-gating alone fixes **none** of the fixtures in the
   table above: each one genuinely needs the header it trips over (the
   `image-*` set needs `_NSGetExecutablePath`, `async-echo-server` needs
   sockets), and the `TargetConditionals` row is fixture-local anyway. Gating
   helps programs that do *not* use a feature; the ones that fall back are
   precisely those that do. Option 2 buys per-program parse time across the
   corpus -- a real J2 goal -- but it will not move this report's count.

Option 1 is the fix for this report; option 2 is the larger J2-era performance
item it points at. Prior art for gating exists (`g_needs_hamt`,
`g_needs_regex_h`, `g_needs_winsock`) if it is taken up.

## Not covered here

`#pragma pack` / `__attribute__((packed))` are silently ignored by c2mir
(parent report, finding 4 -- a *wrong layout* failure mode rather than a
refused input). Verified 2026-07-30 that Turmeric's emitter produces neither
construct, so nothing exercises it today. It is a constraint to respect --
do not start emitting packed structs without checking c2mir first -- not an
active defect.

## Verification

Needs Apple hardware. `bash tests/run-jit.sh` reports the fallback count
directly ("of which N passed via the cc fallback"); the fix should move it from
60 toward Linux's 47 with the pass/fail totals unchanged.

### `CC` must match the compiler that built `tur` -- READ BEFORE MEASURING

Any macOS fallback number taken with a mismatched toolchain is garbage, and it
fails in a direction that looks like a product bug rather than a setup error.

The Debug build links `libturi.a` with `-fsanitize=address`. The engine's
step-6 fallback shells out to `$CC` (`src/main.c:2288`), defaulting to whatever
`cc` is on `PATH`. If `tur` was built with Homebrew LLVM -- which is the
recommended way to dodge the ASan startup deadlock (see CLAUDE.md) -- but `CC`
resolves to Apple's system clang, every fallback link dies on:

```
Undefined symbols for architecture arm64:
  "___asan_version_mismatch_check_v8", referenced from:
      _asan.module_ctor in libturi.a[89](fiber.c.o)
```

Fixtures that should have PASSED via the fallback then FAIL instead. Measured
on 2026-07-30 this understated the fallback count as **31** and reported **30**
spurious failures; re-running the identical tree with
`CC=$(brew --prefix llvm)/bin/clang` gave the true **58 / 3**. Nothing in the
failure text mentions the toolchain.

So: always run the JIT harness as

```sh
CC=$(brew --prefix llvm)/bin/clang bash tests/run-jit.sh
```

with `CC` naming the same compiler passed as `-DCMAKE_C_COMPILER`, and treat
any batch of `httpd-*` failures as a toolchain-mismatch suspect first.
