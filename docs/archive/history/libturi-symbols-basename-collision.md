# `libturi.a` is missing `runtime/symbols.c`

**RESOLVED 2026-07-29.** One line in `src/CMakeLists.txt`: `runtime/symbols.c`
joins `TUR_CORE_SOURCES`. The spike's compile-the-TU-in workaround is reverted.

**The original root cause below was WRONG, and the title with it.** There is no
`ar` basename collision. Everything this report *observed* was accurate; the
explanation it inferred from those observations was not. Kept in full, because
the way a plausible mechanism survived a repro that could not distinguish it
from the real one is the more useful part.

---

## What was actually wrong

`src/runtime/symbols.c` was in `TURT_RUNTIME_SOURCES` and **not** in
`TUR_CORE_SOURCES`. `libturi` is built from `$<TARGET_OBJECTS:tur_core>`, so the
TU was never compiled into that archive at all. It was the *only* member of
`TURT_RUNTIME_SOURCES` not dual-listed -- `hamt.c`, `tur_string.c`, `rc.c`,
`gc.c`, and `rc_free_queue.c` all appear in both lists:

```sh
$ for f in hamt symbols tur_string rc gc rc_free_queue; do
    printf '%-16s %s\n' "$f" "$(sed -n '148,276p' src/CMakeLists.txt | grep -c "runtime/$f\.c")"
  done
hamt             1
symbols          0        # <-- the omission
tur_string       1
rc               1
gc               1
rc_free_queue    1
```

The duplicate definition across the two archives costs existing links nothing:
a static archive contributes only members that resolve an undefined symbol, so
whichever archive the linker reaches first supplies it and the other member is
never extracted. That is precisely how `hamt.c` has always worked.

## Why the collision story was believable

The report's three observations were all real and all consistent with it:

- `ar t libturi.a | grep -i symbol` prints exactly one `symbols.c.o` -- true,
  because only *one* symbols.c is in `tur_core`, not because `ar` dropped one.
- `nm libturi.a | grep -c tur_sym_register` is 0 -- true.
- `nm turt_runtime.dir/runtime/symbols.c.o` shows `T tur_sym_register` -- true.

Every one of those holds under both explanations. The repro in the original
report could not tell them apart, and it was written as though it had. The
check that separates them is one line and was never run: *which* object is the
`symbols.c.o` inside the archive.

```sh
$ find build-jit -name symbols.c.o | while read f; do echo "$f:"; nm "$f" | grep ' T '; done
build-jit/src/CMakeFiles/tur_core.dir/compiler/symbols.c.o:      symtab_init, symtab_free, ...
build-jit/src/CMakeFiles/turt_runtime.dir/runtime/symbols.c.o:   tur_sym_register
```

Two objects, two directories, no contention -- `tur_core` compiled the compiler
one and nothing ever asked it to compile the runtime one.

The proposed fix followed from the wrong cause and would have accomplished
nothing: renaming `src/compiler/symbols.c` removes an ambiguity that was never
the problem, and `tur_sym_register` would still have been absent from
`libturi.a` afterwards. The "worth checking whether any other basename is
duplicated" note is also moot -- there are no duplicate basenames in
`TUR_CORE_SOURCES` (97 sources, zero collisions), which on its own should have
falsified the diagnosis before the fix directions were written.

## Verification

- `nm build-jit/src/libturi.a | grep -c 'T tur_sym_register'` -> 1 (was 0).
- Spike harness relinked with `--whole-archive libturi` and **no** direct
  `symbols.c` compile: `sym-dynamic` runs correctly under the JIT.
- `bash tests/run.sh`: 2399 passed, 0 failed.
- Full JIT corpus sweep: 1645/1680, unchanged from before the revert -- the
  workaround was load-bearing and the fix replaces it exactly.

## Provenance

docs/upcoming/jit-engine-j0-findings.md section 11.5, during the S2
host-symbol-boundary work.
