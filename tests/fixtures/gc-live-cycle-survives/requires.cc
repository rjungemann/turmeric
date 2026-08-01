# Live-heap-bytes assertion: the fixture calls the process-wide allocator
# probe (malloc_zone_statistics on the default zone / mallinfo2) before and
# after a workload and asserts the delta is 0.  That only measures the
# PROGRAM's heap when the program owns its process, which is the cc path.
#
# Under one-process `tur jit` the program shares the compiler's address space
# and its allocator, so the probe reports the compiler's heap too.  On a
# sanitized Debug build -- the configuration CI's JIT job uses -- ASan
# registers itself as the default malloc zone and its quarantine grows
# monotonically, so the delta is large and non-deterministic (measured:
# 800000 for gc-collects-strong-cycle) while the same binary's cc path prints
# 0.  An unsanitized JIT build happens to print 0, but only because the
# compiler's own heap is near steady-state across the loop -- the probe is
# measuring the wrong thing either way.
#
# See docs/reported/jit-macos-gc-rc-weak-fixtures-fail.md.
