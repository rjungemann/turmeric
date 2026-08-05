# Research spike: does the MIR JIT work on Windows?

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
