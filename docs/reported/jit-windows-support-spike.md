# Research spike: does the MIR JIT work on Windows?

> **SPIKE RUN 2026-08-05.** Verdict: **a Windows JIT is real but the road is
> not the one this report assumed.** Building everything was nearly trivial;
> the wall is c2mir versus the MinGW system headers, and the recommended route
> around it is the S2 split-runtime path, not header compatibility. Findings
> below; the original questions follow, annotated.
>
> ## What was done
>
> `-DTUR_JIT=ON` Release build under MSYS2/UCRT64 (gcc 16.1), then
> `tur --enable=jit jit hello.tur` on the result.
>
> - **MIR + c2mir compile unmodified under MinGW.** Nothing in `_deps/mir-src`
>   failed.
> - **`src/jit_engine.c` needed three small fixes** (landed with this update):
>   `<dlfcn.h>` -> the tree's `platform_dl.h` (third site for that remedy);
>   `getrusage` -> `GetProcessMemoryInfo` under `_WIN32`; and `RTLD_DEFAULT`,
>   which `platform_dl.h` did not define -- now implemented there as a
>   process-wide search (main module, then `EnumProcessModules` walk, the
>   dlfcn-win32 approach). Note `ENABLE_EXPORTS` on the `tur` target already
>   maps to `-Wl,--export-all-symbols` on MinGW, so the exported-symbols
>   prerequisite for `dlsym(RTLD_DEFAULT)` was already in place.
> - **c2mir had no Windows system-header path at all** (its baked-in list
>   covers /usr/include and the macOS SDK only). Fixed at the call site, not
>   in MIR: caller include_dirs join c2mir's *system* search, so
>   `jit_sdk_include_dirs` now appends the toolchain include dir found by
>   walking PATH for `cc.exe` (`<bindir>/../include`), with
>   `TUR_JIT_SYS_INCLUDE` as an explicit override.
> - **The engine then runs, engages, parses -- and the fallback machinery
>   works exactly as designed**: on the header wall below it prints TUR-W0070
>   and the cc path produces the right output.
>
> ## The wall: c2mir cannot digest the MinGW headers
>
> With the include path fixed, compilation dies *inside* the UCRT/MinGW
> headers, three distinct ways, in the first few hundred lines:
>
> 1. `vadefs.h:35: #error VARARGS not implemented for this compiler` -- the
>    MinGW headers hard-require GCC or MSVC va_list intrinsics. On Linux/macOS
>    c2mir supplies its own `<stdarg.h>`; on Windows `corecrt.h` pulls
>    `vadefs.h` directly, so there is no own-header route around it.
> 2. `wrong #pragma pack: expected ')'` on `pack(push, _CRT_PACKING)` -- our
>    fork's `#pragma pack` support does not macro-expand the pack argument.
>    A NEW concrete c2mir fork bug, worth fixing regardless of this spike.
> 3. Fatal: `winnt.h:1703: error in opening file x86intrin.h` -- a
>    GCC-internal header, wall-to-wall `__builtin_ia32_*`. Supplying GCC's
>    private include dir would only move the failure inside it. This is not
>    fixable by include paths, and it is reached from `<winsock2.h>` ->
>    `<windows.h>` -> `<winnt.h>` on every program, because the emitted C
>    includes winsock/windows.h under `#ifdef _WIN32` in the preamble.
>
> This is the macOS-SDK class of problem
> (docs/archive/history/jit-macos-apple-sdk-headers-force-cc-fallback.md), for
> a second SDK, and deeper -- the macOS fixes were parse tolerance, where
> vadefs/x86intrin need compiler *intrinsics*.
>
> ## Recommended route: make the emitted JIT TU windows-header-free
>
> The right move is to stop feeding c2mir the Windows SDK, not to teach it the
> SDK. The tree already has the architecture for this: the S2 split-runtime
> path replaces the fixed preamble with committed decls, and the runtime lives
> in the HOST (tur.exe / libturi), where it is compiled by a real GCC and
> resolved via `dlsym(RTLD_DEFAULT)` -- which now works on Windows. What
> blocks it today:
>
> - The split hash-guard disengages on any divergence, and the known
>   [jit-s2-split-disengages-on-hoisted-inline-c-include.md](jit-s2-split-disengages-on-hoisted-inline-c-include.md)
>   plus the `_WIN32` winsock/ucontext emission mean it effectively never
>   engages on Windows.
> - The split TU must also drop the `#ifdef _WIN32` ucontext-shim `__asm__`
>   block and winsock includes (host provides both) -- which incidentally
>   retires original question 3 rather than answering it.
>
> ## Still unanswered
>
> No JIT-generated code has executed on Windows yet, so the MS x64 ABI
> question (1) and executable memory (4) remain open. They are the next
> things the split-runtime route would hit.

---


**Summary:** The JIT is validated on x86-64 Linux and arm64 macOS only. Windows
has never been tried, and it is the platform that would benefit most -- a
working JIT removes the hard requirement that every Windows *user* have MSYS2 +
MinGW installed. Spike it before committing to a plan.

**Severity:** Enhancement / research. Nothing is broken; this is an unexplored
capability with an unusually high payoff on one platform.

**Type:** Timeboxed research spike. Do not start an implementation from this
report -- start a findings doc.

---

## Why this is worth a spike now

`tur build` shells out to a C compiler. On Linux and macOS that is a reasonable
assumption: a developer machine has `cc`. On Windows it is not -- there is no
system C compiler, so today the entire toolchain (compiler, and any program it
builds) requires the user to install MSYS2/UCRT64 first.

The remaining-work plan already names this group -- `tur install`, `tur fetch`,
`tur new`, REPL spice loading -- as "the highest-impact group for an actual
Windows user"
([docs/upcoming/v1/windows-remaining-plan.md](../upcoming/v1/windows-remaining-plan.md)).
A JIT does not merely improve that group; it deletes the premise. c2mir *is* the
C compiler, vendored into `libturi`, so there is nothing external to install.

That makes Windows the platform where the JIT is worth the most, and the one
platform where it has never been run.

## What we know

- **Engine:** MIR (c2mir front end + MIR-gen back end), vendored from the
  `rjungemann/mir` fork pinned at `9c221f96`
  ([cmake/mir.cmake:77](../../cmake/mir.cmake)). Opt-in at build time with
  `-DTUR_JIT=ON`, gated at run time behind the `jit` experiment.
- **Validated platforms:** "J0 COMPLETE (x86-64 Linux + arm64 macOS)"
  ([docs/upcoming/jit-engine-plan.md:3](../upcoming/jit-engine-plan.md)). MIR's
  own claim is "x86-64 + AArch64" (plan, section 2 table) -- note that names
  *architectures*, not OSes, and the Windows x64 ABI is not the SysV one.
- **The Windows build has never enabled it.** `build-win/CMakeCache.txt` carries
  `TUR_JIT:BOOL=OFF`. No Windows JIT build exists to regress.
- **One Windows limit is already documented.** The REPL's JIT path builds a
  shadow symlink directory, and "the symlinks gate this out of Windows"
  ([docs/guides/jit-guide.md:317](../guides/jit-guide.md)). That is the REPL
  path specifically, not the engine.

## Open questions the spike must answer

Ordered so that a "no" high in the list makes the rest moot.

1. **Does MIR-gen implement the Microsoft x64 ABI?** This is the gating
   question. Win64 differs from SysV x86-64 in argument registers
   (RCX/RDX/R8/R9 vs RDI/RSI/RDX/RCX/R8/R9), mandatory 32-byte shadow space, and
   struct-passing rules. If MIR-gen emits SysV on x86-64 unconditionally, then
   Windows is a back-end port, not a configuration -- and the spike should stop
   and say so, loudly, because that is a different order of work.
2. **Can c2mir parse the UCRT / MinGW headers?** There is direct precedent for
   this failing: Apple's SDK headers forced a `cc` fallback on macOS
   ([docs/archive/history/jit-macos-apple-sdk-headers-force-cc-fallback.md](../archive/history/jit-macos-apple-sdk-headers-force-cc-fallback.md)),
   and three of the four c2mir gaps found so far were *silent* wrong answers
   rather than refusals ([jit-guide.md:125](../guides/jit-guide.md)). One of
   those, `#pragma pack`, was fixed in our fork precisely because "the Apple and
   Windows SDKs rely on it heavily" -- so the single riskiest c2mir gap for
   Windows is already closed. That is encouraging, not conclusive.
3. **What happens to the emitted `__asm__` block?** This one is specific and
   newly relevant. The Windows ucontext shim
   ([src/compiler/emit_module.c](../../src/compiler/emit_module.c),
   `emit_win_ucontext_shim`) emits a file-scope GNU `__asm__` block containing
   raw x86-64 assembly plus COMDAT section directives -- and it is emitted into
   *every* generated TU under `#ifdef _WIN32`. c2mir is a C front end; if it
   does not support GNU inline asm (very likely), then **every** JIT compile on
   Windows hits this block. It is not an edge case reachable only by fiber code.

   If so, the fix direction is to gate the shim on the JIT path and supply the
   context-switch primitives as host-provided symbols instead -- `libturi`
   already has them in `src/async/fiber_ctx_x64_win.S`, which is the exact code
   the shim re-emits because "generated C is standalone and cannot link that
   object." Under the JIT it is not standalone: it links against the host.
4. **Executable memory.** Windows wants `VirtualAlloc` +
   `PAGE_EXECUTE_READWRITE` (or a W^X dance) rather than `mmap`. MIR abstracts
   this; confirm the abstraction has a Windows arm. Also confirm whether EDR /
   antivirus interference on RWX pages is a practical problem -- this is a real
   deployment risk on Windows that does not exist on the other two platforms.
5. **Does `#pragma pack` in the UCRT headers actually round-trip?** We have the
   fork fix; nothing has exercised it against real Windows headers.

## Method

- Build with `-DTUR_JIT=ON` from an MSYS2 UCRT64 shell. Expect the MIR fetch and
  build to be the first thing that breaks; MIR's own build is CMake-based but
  has not been configured for MinGW here.
- Smallest possible program first (`(defn main [] : int 0)`), *not* the fixture
  corpus. Questions 1, 3 and 4 all fail on a hello-world, and diagnosing them in
  isolation is far cheaper than inside a suite run.
- Then `tests/run-jit.sh` for breadth, with the same caveat the guide gives: do
  not verify anything in this area with `emit-c` output or a plain
  `tests/run.sh`, because neither exercises the hoist path and both have
  reported a false all-clear on exactly this question
  ([jit-guide.md](../guides/jit-guide.md), findings 21.2/21.3).
- Record in a findings doc under `docs/upcoming/`, in the style of
  [jit-engine-j0-findings.md](../upcoming/jit-engine-j0-findings.md).

## Exit criteria

The spike is done when it can answer, with evidence: *is a Windows JIT a
configuration change, a bounded port, or a MIR back-end project?* A one-line
verdict per numbered question above, plus a recommendation, is the deliverable.
Working code is explicitly not required.

## Related

- [jit-godot-embedding-spike.md](jit-godot-embedding-spike.md) -- the companion
  spike; the two intersect at "a shipped Godot game on Windows."
- [docs/upcoming/v1/windows-remaining-plan.md](../upcoming/v1/windows-remaining-plan.md)
- [docs/guides/jit-guide.md](../guides/jit-guide.md)

---

## 2026-08-05, later: NATIVE execution works; lazy thunks are the last defect

With the compat prelude, export table, and include-walk fixes landed
(`b5ea7628f`), `tur --enable=jit jit hello.tur` on Windows:

- `TUR_JIT_GEN=interp`: runs, exit 0 -- full pipeline proven.
- `TUR_JIT_GEN=eager`:  runs, exit 0 -- **MIR-gen native code executing a
  Turmeric program on Windows.**
- default (lazy):       SIGILL at a low address (thunk page) on the run
  thread, before any program output.

So the one remaining defect is the lazy-generation path -- MIR's
`_MIR_get_wrapper` / `_MIR_redirect_thunk` machinery on the win64 target,
which tur's serialized-lazy interface builds on. Everything below it is
proven by the eager run. Next session: chase the thunk (rjungemann/mir,
branch from `fix/pragma-pack-macro-arg`), or flip the Windows default to
eager as an interim -- the engine comment notes eager "doubles" cost, which
may be acceptable to ship a working tier first.


---

## 2026-09-04: S2 was disengaged for EVERYONE; the lazy fault re-characterized

Going after the lazy-generation crash turned up a larger, cross-platform bug on
the way in, plus two Windows build/CI gaps. The crash itself is better
understood but not fixed.

### The S2 split path was silently off, on every platform

`tur jit hello.tur` on main fell back to the whole-preamble path. The engage
probe explains why:

```
split-debug: probe=4bbd9925fea410ea  committed=7a8360d2df358f58
```

Regenerating the committed artifacts produced **no diff**, so they were not
stale -- the PROBE was emitting different text. Diffing the two showed the
probe carrying an SX1 trail-guard block inside `tur_serial_cont_serialize` that
the canonical emission omits.

That block is gated on `g_trail_autoloaded`, i.e. whether `stdlib/trail.tur`
happened to be autoloaded into *that particular compile*. `cmd_emit_rt_split`
does not autoload it; the JIT probe does. `emit_rt_split_source` forces six
other gates to a canonical posture (`g_needs_hamt`, `g_needs_regex_h`,
`g_has_variadics`, `g_cps_path`, `g_needs_winsock`, and the rc/GC archive
posture) and missed this one.

This is the **second instance of the same hazard** -- the first was the rc/GC
archive flag, fixed in the Windows bring-up. The consequence is worse than it
looks, because nothing reports it: the hash compare fails, S2 disengages, and
the JIT quietly uses the slower whole-preamble path. On Windows that path then
dies on `__va_start` and falls back again to `cc`, which is why `hello.tur`
appeared to "work".

Fixed by forcing `g_trail_autoloaded` in `emit_rt_split_source` and restoring
it on the way out. Safe for the split specifically: the runtime half is
compiled into the host, which always links `src/runtime/trail.c`, so
`tur_trail_level_i64` resolves. The gate's own comment is about the NON-split
path, where a looser gate would emit a call `cc` cannot resolve.

After the fix and a regen, `probe == committed == 4bbd9925fea410ea` and S2
engages.

**Worth a guard.** A stale-or-divergent blob is invisible today. A CI check
that runs the engage probe and fails when it does not match would have caught
both instances the day they landed.

### Two Windows build/CI gaps

- `src/turi/jit_ffi.c` includes `<dlfcn.h>` unguarded, so the Windows JIT build
  does not compile at all on main. Fixed with the same `platform_dl.h` guard
  `jit_engine.c` already carries.
- The `windows` CI job configures **without `-DTUR_JIT=ON`**, so nothing on
  Windows ever compiles the JIT sources. That is how the above reached main.
  The job added during the bring-up guards the default build only.

### The lazy fault, re-characterized

Two corrections to what this report previously recorded:

1. It is **SIGSEGV, not SIGILL**.
2. It is **not** the `PAGE_EXECUTE`-without-READ theory recorded earlier. That
   was a guess and it is wrong: probing
   `VirtualAlloc(NULL, len, MEM_COMMIT, PAGE_EXECUTE)` on this host returns a
   valid pointer with `GetLastError() == 0`, and `VirtualQuery` confirms
   `Protect=0x10`. The allocator is not the fault.

What the fault actually looks like, with S2 engaged so the run reaches it:

```
interp  -> "jit hello", exit 0
eager   -> "jit hello", exit 0
lazy    -> no output, SIGSEGV

Thread 5 received signal SIGSEGV
#0  0x0000000001cb0180 in ?? ()
#1  jit_run_entry ()
=> 0x1cb0180:  mov  %rsp,0x429e8(%rax)
```

The page is readable and executing -- the fault is a **data store through a bad
`%rax`**, not an instruction-fetch fault. Those bytes match neither pattern in
`_MIR_get_wrapper`, and MIR *does* carry a `_WIN32` arm there (and in
`_MIR_get_wrapper_end`), so this is not a missing win64 port at the wrapper
level. The shape is consistent with a jump landing mid-instruction, i.e. a bad
redirect target -- which points at `_MIR_redirect_thunk` or the lazy
serialized-thunk interface built on it.

> **Superseded 2026-09-05 -- see "Resolution" below.** This guess was half
> right and half wrong, and the wrong half is the expensive kind. The redirect
> target WAS correct: `_MIR_redirect_thunk` is not at fault, and neither is the
> serialized-lazy interface. What was broken is the thing the thunk correctly
> jumped TO -- `_MIR_get_wrapper` built a corrupt wrapper. A bad redirect and a
> correct redirect to a corrupt target are indistinguishable from a backtrace;
> only reading the emitted bytes separates them, and reading them took minutes.

Until then `TUR_JIT_GEN=eager` is a working tier on Windows.


---

## Resolution 2026-09-05: three defects in MIR's win64 wrapper assembly

Fixed in the fork ([rjungemann/mir#3](https://github.com/rjungemann/mir/pull/3))
and pinned here. `TUR_JIT_GEN=lazy` -- the **default** tier, which had never
once worked on Windows -- now runs.

### How it was found

Not by single-stepping, which is what the note above proposed. A ~50-line C
program calling the three primitives directly -- `_MIR_get_thunk`,
`_MIR_get_wrapper`, `_MIR_redirect_thunk` -- and **dumping the emitted bytes**
reproduced the SIGSEGV with no turmeric, no generator and no MIR module in the
picture. The defect was legible in the first hexdump. That program is now
`mir-tests/wrapper-abi.c` upstream.

The lesson repeats one this report already records elsewhere: the estimate came
from reading the emitters, the correction came from reading their output.

### Defect 1 -- `_MIR_get_wrapper` patched its immediates 10 bytes short

The win64 `start_pat` opens with two 5-byte stores of `%rcx`/`%rdx` into the
caller's shadow space. The offsets used (`2, 12, 22, 31`) are the ones that
would be right if the pattern began at the first `movabs`.

| immediate | written at | actually landed on |
| --- | --- | --- |
| `called_func` | 2 | both shadow-space stores |
| `ctx` | 12 | inside the `called_func` immediate |
| `hook_address` | 22 | inside the `ctx` immediate |
| tail `rel32` | 31 | inside the `hook_address` immediate |

So the wrapper's first instruction decoded as `mov %rax,(%rax)`. That is the
"data store through a bad `%rax`" recorded above -- it was never a jump landing
mid-instruction. Corrected to `12, 22, 32, 41`.

### Defect 2 -- `_MIR_get_wrapper_end` did not align the stack

Behind defect 1, and invisible until it was fixed. `rsp` is reduced by
`(rsp & 0xf) + C`, which leaves it congruent to `-C` mod 16, so `C` must be a
multiple of 16. It was `0x28`. **The comment on that very line already read**
`add $0x40,%rax` -- the comment was right and the byte was wrong. Any hook using
aligned SSE stores (i.e. any ordinary C function) faulted, which is why fixing
defect 1 moved the crash into `printf` rather than curing it.

### Defect 3 -- `xmm0..3` were saved on top of the callee's shadow space

Behind defect 2, and this one never crashes -- it returns wrong answers. The
saves went to `0..0x20(%rsp)`, exactly the 32 bytes the callee owns, and the
hook spills its own register parameters there. Measured with non-integral
values, so a clobber and a truncation are distinguishable:

```
f(7.25, 3.5, 0.125, 1.0625)   reached the generated code as
f(0,    0,   0.125, 1.0625)   -- got 1.1875, want 11.9375
```

`xmm2`/`xmm3` survived and `xmm0`/`xmm1` did not, which is exactly the first 16
bytes a callee spilling `rcx` and `rdx` overwrites. Moved to `0x20..0x40(%rsp)`,
already covered by defect 2's `0x40` reservation.

`_MIR_get_bb_wrapper` was checked for the same three shapes and is correct on
all of them; only these two functions were wrong.

### Verified

| check | before | after |
| --- | --- | --- |
| `mir-tests/wrapper-abi.c` | SIGSEGV | `wrapper ABI: OK` |
| the same, with only defects 1+2 fixed | -- | `float args: got 1.187500, want 11.937500` |
| `tur jit hello.tur`, `TUR_JIT_GEN=lazy` | SIGSEGV, no output | `jit hello`, rc 0 |
| `tur jit hello.tur`, default (= lazy) | SIGSEGV, no output | `jit hello`, rc 0 |
| `tests/run-jit.sh`, default tier | **0 passed, 5 failed** (5-fixture sample) | **2637 passed, 68 failed, 59 skipped** |

The middle row is the point: it is the evidence that the new test is not one
that cannot fail. It fails three different ways against three different states
of the sources.

The 68 remaining failures contain nothing thunk-related:

- **56 are CPS/effect-shaped** -- the `__builtin_setjmp` gap tracked in section 2
  below. Unchanged in kind from the eager baseline.
- **11 carry `requires.posix-apis`** and are a harness gap, not a defect:
  `run-jit.sh` did not honour the marker. Fixed on a separate branch
  (turmeric#818), not merged when this run was taken, so they are still counted
  here.
- **1 genuinely unexplained**: `path-string`. The earlier count of two was wrong --
  `try-with-basic` is plainly in the effect class.

### CI

`windows-jit` now smoke-tests all three generation tiers on a hello-world. It
still does not run the corpus -- that stays red for the `__builtin_setjmp`
reason below -- but a regression in the default tier would otherwise be
invisible, since `interp` and `eager` were unaffected by all three defects and
a build-only job cannot tell the difference.

---

## Remaining Windows JIT work (2026-09-04)

Everything left, in one place.  Before this section the answer was spread across
a passing clause in the text above, an archived report, and a CI comment -- so a
triage pass could not see it.

### Measured baseline, first ever on Windows

`tests/run-jit.sh` had never been run here.  It has now:

| generation mode | result |
| --- | --- |
| default (**lazy**) | **0 passed, 5 failed** on a five-fixture sample |
| `TUR_JIT_GEN=eager` | **2621 passed, 61 failed, 59 skipped** (full corpus) |

So the corpus *works* on Windows.  `run-jit.sh` uses the default mode, so the
lazy fault below fails it at 100% for a single reason -- which is why "the JIT
corpus does not run on Windows" was true and also misleading.

The 61 eager failures decompose almost entirely into classes already tracked:

- **48 are CPS/effect-shaped** (`cps-*`, perform/resume/handle).  Consistent
  with the c2mir setjmp limitation below -- these are exactly the fixtures that
  exercise the DK trampoline.
- **11 are the POSIX set** (`reactor-*`, `scheduler-io-park`,
  `term-raw-cooked-roundtrip`).  These carry `requires.posix-apis`, which
  `tests/run.sh` honours and **`tests/run-jit.sh` does not** -- a harness gap,
  not a JIT defect.  Cheap to close: teach run-jit.sh the same marker.
- **1 is `fat-dispatch-parametric-monomorph-return`** -- the Win64
  aggregate-return threshold, tracked separately in
  [win64-aggregate-return-threshold-is-sysv.md](win64-aggregate-return-threshold-is-sysv.md).
- **2 unexplained**: `path-string`, `try-with-basic`.

### 1. The lazy-generation fault -- ~~the one blocker that matters~~ RESOLVED

**Fixed 2026-09-05**, see "Resolution" above. Three defects in MIR's win64
wrapper assembly, all in the fork ([rjungemann/mir#3](https://github.com/rjungemann/mir/pull/3)).
The default tier went from **0 passed** to **2637 passed, 68 failed** on the
full corpus, and none of the residue is thunk-related.

The characterisation this section carried -- "not a missing win64 wrapper port,
because MIR *does* carry a `_WIN32` arm there" -- was the wrong inference. The
arm existed and was wrong, which is not the same as absent and is harder to see.

### 2. c2mir has no `__builtin_setjmp`, so effects under the JIT are broken

Documented in
[../archive/windows-longjmp-across-fiber-stack-kills-effects.md](../archive/windows-longjmp-across-fiber-stack-kills-effects.md),
which is ARCHIVED -- so it is invisible to a triage pass over `docs/reported/`,
and that is why this section exists.

The cc path uses `__builtin_setjmp`/`__builtin_longjmp` on Windows because libc
`longjmp` is an SEH unwind that dies on a fiber stack.  Under the S2 split both
halves must agree on the mechanism and c2mir has neither builtin, so the split
keeps plain `setjmp` -- leaving the JIT path with the defect the cc path no
longer has.  The 48 CPS failures above are the visible consequence.

Lifting it means teaching c2mir `__builtin_setjmp`/`__builtin_longjmp` in the
`rjungemann/mir` fork.  That would also let the split use the builtins and
retire the `rt_split_canonical_emission()` special case in the DK prelude.

### 3. `__va_start` on the whole-preamble path

Mentioned above only as the reason `hello.tur` fell back to `cc`.  Stated
plainly here because it is a defect in its own right: when S2 is engaged the
whole-preamble path is unused, but if S2 ever disengages again -- which it did,
twice, silently -- Windows has **no working fallback**, because c2mir cannot
compile the win64 `__va_start` lowering.

Not investigated.  Nothing depends on it while S2 holds, and the engage probe is
now guarded in CI, so the risk is bounded rather than closed.

### 4. `windows-jit` CI is build-only -- now build + smoke

The job added alongside this work compiles the JIT sources on Windows -- which
nothing did before, hence `jit_ffi.c` reaching main with an unguarded
`<dlfcn.h>`. As of 2026-09-05 it also **smoke-tests all three generation tiers**
on a hello-world, which is what guards the fix in (1): a regression there is
otherwise invisible, because `interp` and `eager` were unaffected by all three
defects and a build-only job cannot tell the difference.

It still does not run the corpus. With (1) fixed and the `run-jit.sh` marker
gap closed (turmeric#818), the expected baseline is 2637 + 11 = **2648 passed,
57 failed** -- still red, and red for one tracked reason: (2) below. A
permanently-red job teaches people to ignore it, so the corpus stays off until
c2mir learns `__builtin_setjmp`.
