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
| `empty preprocessor expression` | 3 | `mach/port.h:100`, reached via `sys/socket.h` | `image-hooks-tracked`, `image-reload-hook`, `image-roundtrip` |
| `unresolved import: _OSSwapInt16` | 1 | `libkern/OSByteOrder.h:314` (`#error Unknown endianess`) | `async-echo-server` |

That is the original 9 from the parent report; findings 32.2 measures the
current macOS fallback total at 60 against Linux's 47, i.e. **13** attributable
to the SDK. The extra 4 over the original survey have not been individually
attributed.

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
2. **Stop reaching the headers at all.** The S2 split already removed the fixed
   runtime preamble from every per-program compile. Whatever still pulls
   `sys/socket.h` / `mach/port.h` into a program TU is a candidate for the same
   treatment -- a host-resident declaration rather than an SDK include. This is
   the direction that also buys the per-program parse time, not just these 13.

Option 1 is the fix for this report; option 2 is the larger J2-era performance
item it points at.

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
