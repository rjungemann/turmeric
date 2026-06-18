---
title: Parametric `(Option (NonEmpty A))` return type leaks compiler memory in `clone_struct_app_type`
category: Compiler / Typechecker / Memory hygiene
severity: Medium. The compiler's own ASan run flags a 304-byte leak from
  `clone_struct_app_type` (`src/compiler/types.c:570`) when typechecking a
  `defn` whose return is a doubly-nested parametric TY_APP -- specifically
  `(Option (NonEmpty A))` where both `A` (a body tyvar) and `NonEmpty A` (a
  parameterized defopaque application) are unresolved. The fixture runs
  correctly (program output matches expected), but the compiler leak
  trips the suite's leak-detection gate per CLAUDE.md.
status: OPEN. Repro is `stdlib/refined.tur` `ne-from?` rewritten to pure
  Turmeric with a `(Option (NonEmpty A))` return -- reverted to the carrier
  ABI in this branch with a pointer back to this report.
---

# `clone_struct_app_type` leak on doubly-nested parametric TY_APP return

## Repro

```turmeric
(defn ne-from? [A] [xs : int] : (Option (NonEmpty A))
  (if (tnil? xs)
    (none)
    (some (:: xs (NonEmpty A)))))
```

Build with `./build/tur build`:

- Program output is correct.
- `cc` succeeds and the resulting binary runs.
- ASan flags two 304-byte leaks during the compile:

```
Direct leak of 304 byte(s) in 1 object(s) allocated from:
    #0 ... malloc
    #1 0x... in clone_struct_app_type src/compiler/types.c:570
```

`bash tests/run.sh` consequently fails the fixture as `build failed`.

## Why this case is novel

The step-2 fix in `docs/reported/option-consumer-retype-byvalue.md`
(landed for `option-map`) exercises a SINGLY-nested parametric return
(`(Option B)` where `B` is a body tyvar). `ne-from?` adds a second layer
(`(Option (NonEmpty A))`) where the inner application is itself
parameterized.

The leak fires once per clone of the outer TY_APP carrying the inner TY_APP
payload. A grep of `clone_struct_app_type` in `src/compiler/types.c`
should locate the missing free path (likely the inner TY_APP clone is not
threaded onto the same arena/free list as its parent).

## Validation

- Re-enable `ne-from?` as `(defn ne-from? [A] [xs : int] : (Option (NonEmpty A)) ...)`.
- `bash tests/run.sh 2>&1 | grep "refined-nonempty"` must pass (no
  `build failed` from a compiler ASan report).

## Related

- `docs/reported/option-consumer-retype-byvalue.md` step 4 (the retype
  this report blocks).
- `docs/reported/zero-arg-construct-ground-byvalue-return.md` (the
  sibling step-2 generalization for ground non-generic returns).
- `src/compiler/types.c` `clone_struct_app_type` (the offender).
