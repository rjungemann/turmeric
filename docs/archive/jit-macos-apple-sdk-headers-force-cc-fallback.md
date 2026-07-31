# `tur jit` on macOS: Apple SDK headers push 13 fixtures to the cc fallback

**RESOLVED 2026-07-31.** No remaining cc fallback on macOS is attributable to
an Apple SDK header. The eight fixtures this report tracked all JIT now, and
the corpus fallback count went **31 -> 17**.

Two notes on that number. The enum round turned over six fixtures rather than
the five predicted -- `gc-auto-collects-without-gc-call` reaches
`<malloc/malloc.h>` through the same inline-C heap probe. And the first pass
stopped at 22 and wrongly declared the remainder non-SDK; see the correction
section below, which took it the rest of the way to 17.

Totals across the whole effort: **2414 passed, 0 failed, 47 skipped**, before
and after.

The blockers and how each closed:

| # | Blocker | Fix |
|---|---|---|
| 1 | `__arm64__` defined empty by MIR's prelude | `JIT_PRELUDE` redefine |
| 2 | `__LITTLE_ENDIAN__` undefined -> `OSByteOrder.h:314` | `JIT_PRELUDE` define |
| 3 | `TargetConditionals.h:398 #error` | `JIT_PRELUDE`, the six `TARGET_*` macros clang computes as 1 |
| 4 | C23 `enum : uint64_t` (`malloc/malloc.h:96`) | fork commit `9c5ad5ef` |
| 5 | `#pragma pack` ignored -> `mach/message.h` size asserts | fork commit `d7e19e8d` |
| 6 | leading `__attribute__` on a struct member -> `<dirent.h>` unparseable | fork commit `9c221f96` |
| 7 | `return <void expr>;` in a `void` function (OUR emitter, not c2mir) | `emit_fns.c` |

Rows 4, 5 and 6 are c2mir changes in `rjungemann/mir`; `cmake/mir.cmake` is
repinned from `90633091` to `9c221f96`. Row 7 is a turmeric-side codegen fix.

**Repinning needs a fresh build dir** --
the cache-variable trap documented in that file is real; verify with
`git -C <dir>/_deps/mir-src log --oneline -1`.

## Correction (2026-07-31): the "none SDK-attributable" claim below was wrong

When this was first marked resolved at 22 fallbacks, the remaining set was
categorised by grepping each fixture's diagnostics for `undefined item` /
`error:` / `#error`. Four fixtures -- `io-stdlib-roundtrip`,
`recursive-linear-borrow-branch`, `tmpfile-linear`, `tmpfile-linear-borrow` --
reported `undeclared identifier d`, which matched none of those patterns, so
they were miscounted as "unrelated c2mir strictness".

They were **SDK header failures after all**, and the symptom is why it was
missed: `<dirent.h>:84` is `__unused long __padding;`, `sys/cdefs.h:172`
defines `__unused` unconditionally as `__attribute__((__unused__))`, and c2mir
accepted a member attribute only *after* the declarator. The whole `DIR` struct
therefore failed to parse -- and because the failure is at the typedef, the
error surfaces far away as `undeclared identifier d` at every later
`DIR *d = opendir(...)`, with nothing pointing at the attribute.

Fixed in fork commit `9c221f96` (one call, matching the idiom `declaration`
already used). Fallbacks **22 -> 18**.

A fifth, `panic-trace`, was correctly categorised as non-SDK but turned out to
be **our** bug rather than a c2mir gap: `(defn outer [] : ! (inner))` emitted
`return inner();` inside a `void` function, which C11 6.8.6.4p1 makes a
constraint violation even when the expression is void-typed. clang accepts it
as an extension so the cc path never complained. Fixed in `emit_fns.c` (emit
`(void)(expr); return;`), snapshot regenerated. Fallbacks **18 -> 17**.

The lesson: categorising by grepping for known error spellings silently drops
whatever spells its failure differently. Read every distinct message, not the
ones you expect.

## What is left

17 fallbacks remain, and these genuinely are not SDK header failures:

- **16 are the `__atomic_*` / `__sync_*` family** (`arc-*`, `atomic-*`,
  `cancel-*`, `future-*`, `once-basic`, `promise-linear`, `workstealing-*`,
  `httpd-async-*`). Deliberate -- see
  `src/jit_engine.c:58-62`. These builtins are type-generic and MIR binds one
  name to one signature, so any single shim writes the wrong width somewhere.
  The cc fallback is the correct behaviour for this class.
- **1 is `thread-local-basic`** -- `_Thread_local`, which c2mir does not
  support. Same category as the atomics: the spike's `#define __thread`
  workaround is deliberately absent because collapsing a thread-local to a
  global trades a clean compile error for silent cross-thread corruption. The
  cc fallback is the correct behaviour.

## Lessons worth keeping

- **Do not advertise `__GNUC__`** to get past `TargetConditionals.h`. It
  suppresses the `#error`, then unlocks GCC-only spellings elsewhere in the SDK
  that c2mir cannot parse (`__header_always_inline` at
  `sys/_types/_fd_def.h:59` is the first to bite). Setting `TARGET_*` directly
  is the header's own documented workaround.
- **Only the `TARGET_*` macros clang computes as 1 are defined.** An undefined
  macro already evaluates to 0 in `#if`, so the ~30 zero-valued ones buy
  nothing and a short list is easier to keep honest.
- **`tur jit -I` does not reach c2mir** -- only `jit_sdk_include_dirs`
  (`src/main.c:3364`) does, which is what made the ceiling experiment possible.
- **Fix direction 1 was a false lead, and diagnosing that was the whole job.**
  Predefining macros cleared both `#error`s and moved the fallback count by
  exactly zero, because each error was masking a deeper c2mir gap. The
  ceiling experiment -- neutralising every blocker by hand before writing any
  compiler code -- is what turned a guess into an ordered plan.

---

## Original report follows

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

**All three surface errors above are now gone; the table records the symptoms
as first observed.** The real blockers were one layer down -- see the resolution
sections at the top of this file before acting on anything below.

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

## Fix direction 1 was tried and does NOT clear either class (2026-07-30)

Measured on arm64 macOS. Predefining the macros works exactly as predicted at
the *preprocessor* level -- and buys nothing, because each `#error` was masking
a deeper c2mir gap that a `-D` cannot touch. **The fallback count did not move:
2414 passed, 0 failed, 47 skipped, 31 via cc fallback, before and after.**

Mechanism note for anyone retrying this: `c2mir_options.macro_commands` is
processed in `compile_init`, but `add_standard_includes` pushes MIR's own
target prelude afterwards and the stream stack reads it FIRST, so a `-D` there
is overwritten by any prelude `#define` of the same name. Defining the macro in
`JIT_PRELUDE` instead is what works -- it is prepended to the emitted TU, which
c2mir reads after its own prelude and before the SDK headers.

- **`mach/port.h` row (`image-*`, 3 fixtures) -- root cause found, and it is a
  MIR bug, not a compiler-identity probe.** `mirc_aarch64_linux.h:135` spells
  `#define __arm64__` with no replacement list, so `#if __arm64__` expands to a
  bare `#if`. `JIT_PRELUDE` now restores it to `1` (shipped). The
  "empty preprocessor expression" is gone -- and what it was hiding is that
  the block it guards defines `xnu_static_assert_struct_size`. With the
  assertions re-armed, `mach/message.h:543`/`:569` now fail
  `static assertion failed: "struct changed size unexpectedly"`, because c2mir
  ignores the `#pragma pack(push, 4)` those structs live inside. Still a
  fallback, now for a **correct** reason. See
  [jit-c2mir-ignores-pragma-pack.md](jit-c2mir-ignores-pragma-pack.md) -- the
  more serious finding this investigation turned up.

- **`TargetConditionals.h` row (gc/hkt/weak, 5 fixtures) -- blocked behind a
  C23 feature.** `-DTARGET_CPU_ARM64=1` (the header's own documented
  workaround, `TargetConditionals.h:393`) does suppress the `#error`. The next
  thing the parse hits is `malloc/malloc.h:96`:
  `typedef enum __enum_options : uint64_t {...}` -- a C23 enum with a fixed
  underlying type, which c2mir cannot parse. Confirmed with a minimal probe
  (`typedef enum : unsigned long long { E_A = 1u } t;` -> `syntax error on
  typedef` under the JIT, `sizeof` 8 under cc). No `-D` reaches this; it needs
  c2mir grammar work.

  `TARGET_CPU_ARM64` was **deliberately not shipped**. It buys no fixture (the
  malloc.h parse error blocks them anyway) and it leaves `TargetConditionals.h`
  half-configured: the `#else` branch guards only the `#error`, so
  `TARGET_OS_MAC` and friends stay undefined and any later `#if TARGET_OS_MAC`
  silently evaluates 0. Adding a known-inconsistent macro set for zero measured
  gain is the wrong trade -- if this is revisited, define the full
  `TARGET_CPU_*`/`TARGET_OS_*` set, not one macro.

So the remaining 8 fixtures are gated on two c2mir capabilities --
`#pragma pack` and C23 enum base types -- not on compiler identity. Fix
direction 1 is closed.

## Ceiling experiment: all 8 fixtures ARE recoverable (2026-07-30)

Rather than guess at what a fork change would buy, each blocker was neutralized
by hand and the fixtures re-run. Method: patched copies of `malloc/malloc.h`
(the `: uint64_t` deleted) and `mach/port.h` (the `_Static_assert` body replaced
with `0 == 0`) dropped into `src/`, which `jit_sdk_include_dirs`
(`src/main.c:3364`) puts on c2mir's search path ahead of the SDK -- note
`tur jit -I` does **not** reach c2mir, only the cc fallback -- plus
`TARGET_CPU_ARM64` and `__LITTLE_ENDIAN__` in `JIT_PRELUDE`.

**Result: all 8 compile and run under the engine, no fallback.** So there is a
real 8-fixture prize here; nothing is fundamentally out of reach.

The full blocker chain, in the order each fixture hits it:

| # | Blocker | Blocks | Status |
|---|---|---|---|
| 1 | `__arm64__` defined empty by MIR's prelude | image-* (3) | **DONE** (`JIT_PRELUDE`) |
| 2 | `__LITTLE_ENDIAN__` undefined -> `OSByteOrder.h:314 #error Unknown endianess` | image-* (3) | **DONE** (`JIT_PRELUDE`) |
| 3 | `TargetConditionals.h:398 #error` | all 8 | **DONE** (`JIT_PRELUDE`, the 6 macros clang computes as 1) |
| 4 | C23 `enum : uint64_t` (`malloc/malloc.h:96`) | gc/hkt/weak (5) | **OPEN -- c2mir grammar, fork** |
| 5 | `#pragma pack` ignored -> `mach/message.h:543,569` size asserts | all 8 | **DONE** -- fork commit `d7e19e8d` |

**Rows 1, 2, 3 and 5 landed 2026-07-31.** The three `image-*` fixtures now JIT;
corpus fallbacks went 31 -> 28 with totals unchanged at 2414 passed / 0 failed.
Row 4 is the only thing still holding the five gc/hkt/weak fixtures, and it is
now the *sole* blocker for them -- with the enum syntax patched out by hand
those five compiled and ran (the ceiling experiment below).

Two notes for whoever picks up row 4:

- Do **not** advertise `__GNUC__` to get past `TargetConditionals.h`. It does
  suppress the `#error`, but it then unlocks GCC-only spellings elsewhere in the
  SDK that c2mir cannot parse -- `__header_always_inline` at
  `sys/_types/_fd_def.h:59` is the first to bite. Setting the `TARGET_*` macros
  directly, which is what the header's own comment recommends, is why row 3
  shipped.
- Only the six `TARGET_*` macros clang computes as **1** are defined. The ~30 it
  computes as 0 are omitted on purpose: an undefined macro already evaluates to
  0 in `#if`, so defining them buys nothing and a short list is easier to keep
  honest.

### The ordering conclusion that mattered

**`#pragma pack` (row 5) was the keystone: it blocked all 8, so until it landed
no other fix on this list bought a single fixture.** That is why
`__LITTLE_ENDIAN__` and `TARGET_CPU_ARM64` were initially declined -- correct
but inert, and inert changes that assert things about the target are pure
downside. Once row 5 landed they became live and shipped alongside it, which is
what turned 3 fixtures over. Row 4 was separately confirmed sufficient for `malloc.h` --
with only the enum syntax patched, that header parses clean, so no further
c2mir gap hides behind it.

Note rows 4 and 5 are both changes to `c2mir/c2mir.c` in the
`rjungemann/mir` fork (`cmake/mir.cmake`), and row 5 in particular touches the
preprocessor, the parser, and struct layout: pragmas are consumed in
`check_pragma` (`c2mir.c:2545`) and never reach the parser today, and
preprocessing completes fully before parsing, so the packing state has to be
carried across that boundary -- either as a marker token in the output stream
or as a position-keyed side table consulted from `set_type_layout`
(`c2mir.c:6220`). Member alignment is clamped in `update_field_layout`
(`c2mir.c:6154`).

## Fix directions

1. ~~**Predefine what the headers probe for.**~~ **Tried; closed.** See the
   section above. Cleared both `#error`s and moved the fallback count by zero.
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
