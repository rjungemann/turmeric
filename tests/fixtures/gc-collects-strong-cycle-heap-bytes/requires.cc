# Byte-level leak backstop: reads the PROCESS heap (malloc_zone_statistics /
# mallinfo2), which equals the PROGRAM's heap only on an unsanitized `cc` build
# that owns its own process.
#
# Under one-process `tur jit` the program shares the compiler's address space
# and allocator, so the probe reads the compiler's heap; on a sanitized build
# it reads the sanitizer's (blind on glibc, quarantine-inflated on Darwin --
# measured 800000 here).  See docs/archive/history/
# gc-leak-gate-darwin-sanitized-probe-drift.md.
#
# The assertion that DOES run under the JIT lives in the sibling fixture
# gc-collects-strong-cycle, which reads the CG6 (gc-live-blocks) counter.
