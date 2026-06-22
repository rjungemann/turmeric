---
title: Module-private (`__`-prefixed) helpers reached cross-module are not emitted/retained -- undefined-reference at the C link step (separate-compilation / DCE retention)
category: Separate compilation / dead-code-elimination retention (NOT carrier<->concrete ABI)
severity: Medium. Three `frame` spice test files fail at the C link step
  (`cc`/`ld`), not at type-check and not at runtime: each references a
  module-private double-underscore helper (e.g. `frame__sort____so_take`,
  `frame__interop____ip_*`) that the importing translation unit calls but that
  is never emitted/retained in the linked program. `tur check` on every
  `src/*.tur` is clean, so this is a codegen / separate-compilation / retention
  problem, not a typing one. Blocks a fully-green `frame` suite against
  tip-of-main `tur`, and will surface as a red CI job (CI builds `tur` from
  turmeric main).
status: RESOLVED 2026-06-22 -- MISDIAGNOSED; NOT a turmeric defect. The symbol is
  NOT dropped/DCE'd: `frame/sort/__so-take` is emitted normally as
  `static int64_t frame__sort___un_unso_hytake(...)` (verified -- it is a real
  definition in the emitted TU, reached by sort.tur's own callers). The link
  failure is a STALE HAND-SPELLED MANGLED NAME in the spice's inline-C bridge:
  group.tur/filter.tur declare `extern int64_t frame__sort____so_take(...)`, but
  the live, self-consistent mangler escapes a literal `_` as `_un` and `-` as
  `_hy` (`src/compiler/mangle.c:11-21`, last touched #457 -- unchanged since),
  so `frame/sort/__so-take` mangles to `frame__sort___un_unso_hytake`, never
  `frame__sort____so_take`. The bridge calls a name that was never emitted under
  this scheme. Same class for the `frame/interop` `__ip_*` Arrow release
  callbacks (hand-spelled by-address C-ABI names). The turmeric-side mechanisms
  are sound: `#[used]` (#467) controls linkage, and the `__TUR_CNAME_<name>__`
  splice reproduces the live mangler -- the c-integration guide already warns
  against hand-spelling mangled names for exactly this reason. The fix is
  spice-side and belongs in turmeric-spices, not turmeric. Verified: exporting
  `__so-take` from `frame/sort` and replacing the two inline-C bridges with a
  normal `(__so-take col perm n)` call makes `reshape_test` (12/12) and
  `group_test` (7/7) pass against this branch's `tur`. See
  `docs/archive/history/cross-module-private-helper-dropped-at-link.md`.
---

# Cross-module private helper dropped at the C link step

## One-line summary

A module-private (`__`-prefixed) helper that *is* referenced across a module
boundary gets dead-code-eliminated / not emitted into the final link unit, so
the importing translation unit's call resolves to an undefined symbol at `ld`.

## Affected tests and the exact missing symbols

```
tests/frame/reshape_test.tur
  undefined reference to `frame__sort____so_take'

tests/frame/group_test.tur
  undefined reference to `frame__sort____so_take'

tests/frame/interop_test.tur
  undefined reference to `frame__interop____ip_release_schema'
  undefined reference to `frame__interop____ip_release_array'
  undefined reference to `frame__interop____ip_fmt_to_tag'
  ... (several more `frame__interop____ip_*` helpers)
```

All are mangled private symbols: `frame/sort`'s `__so_take` (a private helper
reached cross-module from group/reshape) and `frame/interop`'s `__ip_*` helpers
(reached from the interop test).

## Reproduction

```sh
# tur built from turmeric main @ 99cc8b32
cd spices/frame
TUR_STDLIB_DIR=/path/to/turmeric/stdlib tur test tests/frame
# => 11 tests, 8 passed, 3 failed
#    FAIL tests/frame/group_test.tur
#    FAIL tests/frame/interop_test.tur
#    FAIL tests/frame/reshape_test.tur

# Single-file repro with the underlying error:
TUR_STDLIB_DIR=/path/to/turmeric/stdlib tur run tests/frame/reshape_test.tur
# tests_frame_reshape_test_tur.c:(.text+0x23db):
#   undefined reference to `frame__sort____so_take'
# collect2: error: ld returned 1 exit status
# tur: cc invocation failed (status 256)
```

## Confirmation it is NOT the U3 frame/typed change

```sh
cd spices/frame
mv tests/frame/typed_test.tur /tmp/typed_test.bak   # remove the new file
tur test tests/frame
# => 10 tests, 7 passed, 3 failed
#    FAIL group_test / interop_test / reshape_test   (identical 3)
mv /tmp/typed_test.bak tests/frame/typed_test.tur
```

The new `frame/typed` module and its test pass (4/4); they neither cause nor
touch these failures.

## Likely cause

The missing symbols are private (`__`-prefixed) helpers consumed across a module
boundary or reached only via a mangled C symbol. Same class as prior fixes:

- #467 "Add `#[used]` attribute to retain defns reached only via mangled C symbol"
- #465 "Fix two separate-compilation codegen blockers for spices"

Most plausibly a retention/DCE or separate-compilation regression (or an
as-yet-uncovered case of the same class) at tip-of-main, where a private helper
that *is* referenced gets dead-code-eliminated or not emitted into the final
link unit.

## Suggested next steps

1. Reduce `reshape_test.tur` to a minimal cross-module call into `frame/sort`'s
   private `__so_take` and confirm the undefined-reference at that boundary.
2. Bisect turmeric `main` to find where `__so_take` stopped being
   emitted/retained between the last-known-green `tur` for `frame` and
   `99cc8b32`.
3. If it is the `#[used]`/DCE class, the fix belongs in turmeric (this report),
   not in the spice.

## Relationship to the carrier<->concrete ABI audit

**Different family.** The carrier audit
(`docs/carrier-concrete-abi-crossing-audit-plan.md`) is about a parametric
payload's concrete element collapsing to the int64 carrier at an ABI crossing --
a *type/value-representation* defect that miscompiles or crashes a program that
*does* link. This is a *symbol retention / separate-compilation* defect: the
types are fine, the program never links. It is deliberately **not** added as a
`G`-row to that audit (doing so would dilute its single-defect thesis). Its
lineage is #465/#467, and it should be tracked and bisected on that track. Noted
here only so the cross-reference is explicit and the report is not mistaken for
another carrier crossing.
