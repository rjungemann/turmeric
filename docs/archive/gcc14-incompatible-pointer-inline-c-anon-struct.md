# GCC >= 14: hand-written inline-C double-spells anonymous structs (incompatible-pointer-types)

> **Status:** RESOLVED 2026-07-19. All three inline-C sub-patterns fixed: RHS
> struct-pointer-cast double-spell -> `(void *)` (any cast, incl. `malloc`);
> var-to-var assignment between separately-declared identical anon structs ->
> per-block `typedef`; and the `timespec` shadow typedefs (future/taskgroup/
> scheduler) dropped for the system `struct timespec`. A full-tree
> `-Werror=incompatible-pointer-types` sweep now reports **0** failing fixtures;
> full suite green (2202 passed, 0 failed). Commits: `348a25e`, `8513e55`.
>


**Severity:** medium -- latent today (masked by
`-Wno-error=incompatible-pointer-types` in `src/main.c`), a hard `cc` error under
GCC >= 14. The mechanical front split out of `codegen-gcc14-permerrors.md`.

## Summary

Several fixtures (and the `future`/`thread` stdlib inline-C they exercise)
hand-write an anonymous struct type twice -- once in a variable declaration and
again in the cast that initializes it:

```c
struct { bool is_some; int64_t value; } *p =
    (struct { bool is_some; int64_t value; } *)(intptr_t)o;
```

In C, two textually-separate anonymous `struct { ... }` specifiers are **distinct
types**, so the initializer's `struct <anon#2> *` is an incompatible pointer type
for the `struct <anon#1> *` variable -- `-Wincompatible-pointer-types`, promoted
to a hard error under GCC >= 14. The layouts are identical, which is why it runs
fine today and only warns pre-GCC-14.

## Repro

```sh
tur emit-c tests/fixtures/hkt-do-m/input.tur \
  | cc -x c -c - -o /dev/null -Werror=incompatible-pointer-types -Wno-implicit-function-declaration
```

```
tests/fixtures/hkt-do-m/input.tur (inline-C), lines ~27-28:
  struct { bool is_some; int64_t value; } *p =
      (struct { bool is_some; int64_t value; } *)(intptr_t)o;
```

## Affected fixtures (~26)

Almost entirely the HKT family that copies the same hand-written Option/Monad
inline-C helpers: `hkt-do-m`, `hkt-do-m-option`, `hkt-do-m-result`,
`hkt-binary-ctor`, `hkt-closure-capture`, `hkt-closures`, `hkt-closures-defers-refs`,
`hkt-dispatch-unambiguous`, `hkt-fn-constraints`, `hkt-fn-implicit-kind`,
`hkt-fn-kind-param`, `hkt-for-comprehension`, `hkt-for-comprehension-vec`,
`hkt-functor-laws`, `hkt-functor-option`, `hkt-monad-laws`, `hkt-monad-option`,
`hkt-multi-capture-hkt`, `hkt-single-capture-hkt-regression`, `hkt-stdlib-suite`,
`hkt-witness-basic`, `hkt-witness-multi-instance`, `kinds-inference`, plus the
`future-*` fixtures (`future-capturing-closure`, `future-linear`,
`future-split-free`) via the `future`/`thread` stdlib inline-C.

## Fix (mechanical, no codegen change)

Replace the re-spelled struct pointer cast with a `void *` cast, which is
compatible with any object pointer and warning-clean -- exactly what
`stdlib/seq/builders.tur` already does:

```c
struct { bool is_some; int64_t value; } *p = (void *)(intptr_t)o;
```

Or hoist a single shared `typedef` and reference it in both positions. The change
is a value-preserving no-op (same bits, same layout); it only silences the
type-checker. Some of these fixtures carry `expected.c` snapshots that will need
regenerating (the inline-C is emitted verbatim); regenerate them in the same
change per the fixture-regen policy.

Note the compiler already emits a clean `tur_option_t` typedef in its preamble
(`emit_module.c`); this report is only about the HAND-WRITTEN inline-C in fixture
`.tur` files and the `future`/`thread` stdlib inline-C, not compiler-generated
code.

## Note

One of three remaining fronts under the umbrella
`docs/archive/codegen-gcc14-permerrors.md` (the others:
`gcc14-int-conversion-cps-fn-value-dispatch.md` and
`gcc14-int-conversion-carrier-to-typed-param.md`). The two `-Wno-error` flags in
`src/main.c` drop only once all three are clean tree-wide.
