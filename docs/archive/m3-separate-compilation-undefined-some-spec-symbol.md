---
title: m3-separate-compilation fails to link -- undefined `some___spec__bool_Option__opaque`
category: Codegen -- separate compilation / monomorphized constructor spec emission
severity: Low-Medium. Auxiliary `tests/run-flags.sh` only; the default
  `tests/run.sh` gate is green. The separate-compilation flag fixture fails at
  the link step with an undefined reference to a monomorphized `some` spec
  symbol, so `-Xseparate-compilation` (or the m3 path it exercises) does not
  emit/define every `some__spec__...` it references across translation units.
status: RESOLVED
---

## Resolution

Root cause: a prelude/stdlib function (here Option's `some?`) is injected as a
full `FnDef` into *every* project TU, so the J3/J4 ownership check in
`emit_implementation` (`src/compiler/emit_module.c`) -- which marked every spec
whose `fn_expr != NULL` as externally linked -- fired in *every* TU. Each TU
then emitted `some___spec__bool_Option__opaque` with external linkage. Depending
on the build path this surfaced either as the original *undefined reference* (no
single owner emitted it) or, by the time of this fix, as *multiple definition*
(every TU emitted it -- the symptom seen on `frame`/`watch`/`stats`).

The borrow/owner machinery that hands a spec to exactly one owning TU keys on
`binding->defining_module_name`; a genuine user spec needing cross-TU linkage
always carries a non-NULL owner, whereas prelude functions have none
(`defining_module_name == NULL`). Fix:

- In the J3/J4 block, set `external_linkage` only when the spec has an owner
  module (`defining_module_name != NULL`); no-owner (prelude) specs are emitted
  `static` -- a link-safe per-TU copy, identical to how the closure/fat runtime
  is duplicated per TU.
- In `emit_header`, skip the non-static prototype for no-owner specs so the
  static definition does not trip "static declaration follows non-static
  declaration".

Verification: `tests/run-flags.sh` -> 77 passed, 0 failed (the
`m3-separate-compilation` fixture now PASSES); `tests/run.sh` -> 1724 passed,
0 failed; `tur build .` on `frame`, `watch`, `stats` link cleanly.

---

## Repro

```sh
cmake --build build -j --config Debug
TUR_BIN=./build/tur TUR=./build/tur ASAN_OPTIONS=detect_leaks=0 \
  bash tests/run-flags.sh
```

Observed (clean tree, no local changes):

```
FAIL m3-separate-compilation -- build failed (rc=2): /usr/bin/ld: /tmp/ccXXXXXX.o: in function `some___spec__bool_Option__opaque':
tur: cc invocation failed (status 256)
flags summary: 76 passed, 1 failed
```

## Notes

- Confirmed pre-existing: stashing all local work and rebuilding reproduces the
  identical linker error, so it is unrelated to the interpreter-side fixes
  landed alongside this report.
- The undefined symbol is a monomorphized `some` constructor spec for
  `(Option opaque)` with a `bool`-flavored mangling
  (`some___spec__bool_Option__opaque`). Under separate compilation the
  *referencing* TU emits an extern use but no TU emits the definition (or it is
  emitted `static`/with a divergent mangling in the defining TU).

## Fix directions

1. Find where `some__spec__...` constructor specs are emitted
   (`src/compiler/emit_module.c` / `emit_fns.c`) and confirm that under
   `-Xseparate-compilation` the spec is emitted with external linkage in the TU
   that owns it, and that the referencing TU's extern mangling matches byte for
   byte.
2. Check whether the `bool_` element-mangle component on an `(Option opaque)`
   spec is consistent between the definition site and the call site (a mangling
   divergence would produce exactly this "defined here, referenced there as a
   different name" link error).
