# `c_num_spelling` read uninitialized on some `Type` construction paths

**Severity:** low (codegen-snapshot fragility; no observed runtime miscompile)

**Status:** open as of v0.25.0. No `type_make`/zeroing helper has been
introduced; `c_num_spelling` is still set only on the paths in
`elab_types.c:481`/`:1922` and read by `type_c_name`
(`src/compiler/types.c:2448`). The snapshots regenerated alongside
assoc-types-2 remain correct for the current arena layout, so the latent
fragility has not resurfaced -- but the root cause is unchanged.

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
