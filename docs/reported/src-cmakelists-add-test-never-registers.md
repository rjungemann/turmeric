# `add_test` in `src/CMakeLists.txt` never registers, silently

**Severity: low, but the failure mode is the bad kind** -- a test that looks
registered, builds, passes when run by hand, and is never run by CI.

`enable_testing()` is called in the top-level `CMakeLists.txt` at line 119.
`add_subdirectory(src)` is at line 86. CMake only collects tests from a
subdirectory when testing was enabled *before* that subdirectory was added, so
every `add_test` inside `src/CMakeLists.txt` is dropped on the floor.

There is no diagnostic. `cmake` succeeds, the test executable builds, running it
by hand passes, and `ctest` simply never mentions it.

## Repro

```sh
cmake -S . -B build && cmake --build build -j
cd build && ctest -N | tail -3
```

Observed: `Total Tests: 130`, and `tur_trail` is not among them, though
`src/CMakeLists.txt:1003` says:

```cmake
add_test(NAME tur_trail COMMAND tur_trail)
```

```sh
grep -c tur_trail build/CTestTestfile.cmake   # => 0
```

## Scope

One test today: `tur_trail` (`src/CMakeLists.txt:1003`). It has never run in
CI. Whether it currently passes is unknown -- that is the point.

Found 2026-09-05 while adding `source_literal_unit`, which was registered the
same way and was equally invisible. That one has been moved to the top-level
`CMakeLists.txt` next to the other 130; `tur_trail` was left where it is,
annotated, because moving it makes a never-run test start running and that
deserves to be its own change with its own result.

## Fix directions

1. **Move `enable_testing()` above `add_subdirectory(src)`** in the top-level
   file. One line, fixes the class. But it switches `tur_trail` on, so it needs
   to be landed with whatever `tur_trail` then reports.
2. **Move the `add_test` up** to the top-level file, as `source_literal_unit`
   now is. Leaves the trap in place for the next person.
3. Either way, a guard is cheap: assert `ctest -N` reports the expected count,
   or grep `CTestTestfile.cmake` for each `add_test` name declared under `src/`.
   Without one, the next test added there is invisible again.

Direction 1 plus a guard is the real fix. Direction 2 is what has been done so
far, and is not enough on its own.
