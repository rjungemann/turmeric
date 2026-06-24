# `c_num_spelling` read uninitialized on some `Type` construction paths

**Severity:** low (codegen-snapshot fragility; no observed runtime miscompile)

**Status:** RESOLVED. The root cause was the bare `Type t;` declaration in
the inline `Type` constructors in `src/compiler/types.h` -- `type_fn`,
`type_ref`, `type_lref`, `type_rc`, `type_rc_struct`, `type_weak`,
`type_ref_immut`, `type_ref_mut`, `type_typeclass`, `type_typeclass_inst`,
`type_exception`, and `type_cloneable_cont` -- which set fields individually
and never touched `c_num_spelling` (nor, in some cases, `hkt_kind` /
`substruct`). A `Type` produced by `type_fn` (the `>>>` lambda's captured
fat-pointer carrier in the repro) therefore read back a stale, layout-sensitive
`c_num_spelling` byte that `type_c_name` (`src/compiler/types.c:2448`) honored,
leaking a spurious `size_t`/`ptrdiff_t` spelling.

Fix: every one of those constructors now zero-initializes via `Type t = {0};`.
Because `CNUM_DEFAULT == 0` and `KIND_STAR == 0`, the zero default *is* the
correct default for every field, so `c_num_spelling` reliably reads
`CNUM_DEFAULT` (and the previously-uninitialized `hkt_kind`/`substruct` paths
get their correct defaults too). The two affected snapshots stay at the correct
`int64_t` spelling regardless of arena layout, and the full suite is green
(1786 passed, 0 failed).

## Summary

`Type.c_num_spelling` (the `CNumSpelling` byte that makes an integer carrier
spell as `size_t` / `ptrdiff_t` instead of `int64_t` in C output) is not
initialized on every path that builds a `Type`. Because `arena_alloc` does not
zero memory, the byte can read back as a stale non-`CNUM_DEFAULT` value, which
leaks a spurious `size_t`/`ptrdiff_t` spelling into generated C. The value read
is sensitive to arena allocation layout, so unrelated struct-size changes can
flip it.

## Repro

Before the assoc-types-2 work, `tests/fixtures/arrow-compose-float/expected.c`
and `tests/fixtures/sf-compose-typed/expected.c` contained a specialization of
the anonymous `>>>` body whose two captured `^fat` function-pointer params --
which must have identical carriers -- were spelled asymmetrically:

```c
static void * _____spec__void___int64_t_size_t(int64_t f, int64_t g) { ... }
```

The second carrier `size_t` is nonsensical for a fat-pointer capture; neither
fixture uses a `usize`/`size`-named type (the only legitimate sources of
`CNUM_SIZE_T`, see `c_num_spelling_for_name` in `src/compiler/elab_types.c:251`).

Adding three fields to `struct TypeClass` (the assoc-types-2 fundep masks)
perturbed arena layout enough that the stale byte now reads `CNUM_DEFAULT`, and
the carrier correctly spells `int64_t`:

```c
static void * _____spec__void___int64_t_int64_t(int64_t f, int64_t g) { ... }
```

Both fixtures produce identical, correct runtime output in either case
(`7 / 1.5 / 9` and `8 / 10`); only the snapshot moved. The snapshots were
regenerated to the correct `int64_t` spelling alongside that change.

## Root cause (direction)

Some `Type` value used for the `>>>` lambda's captured-param carriers is built
without setting `c_num_spelling = CNUM_DEFAULT`. Candidates: the closure
env/spec carrier-Type construction and any `Type` built by raw `arena_alloc`
(or stack `Type` with designated-init that omits the field) rather than copied
from a fully-initialized source. `type_c_name` (`src/compiler/types.c:2448`)
then honors the garbage byte.

## Fix directions

- Initialize `c_num_spelling = CNUM_DEFAULT` at every `Type` construction site,
  or introduce a single `type_make`/zeroing helper and route allocations
  through it.
- Cheap defensive option: have the spec/closure-carrier path explicitly clear
  `c_num_spelling` on synthesized carrier Types (it is never meaningful for an
  erased int64 fat-pointer carrier).

Until then, the regenerated snapshots are correct for the current layout but may
drift again if `struct TypeClass`/`Type` sizes change.
