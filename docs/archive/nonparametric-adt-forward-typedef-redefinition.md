# A non-parametric ADT's forward typedef re-declares its already-emitted typedef name

> **RESOLVED 2026-09-25.** The third emitter was `emit_module.c`'s early-file
> ADT pass, which wraps a layout in the `TUR_TD_` guard only when the by-value
> sum ordering needs it (`td_guard`).  When it does not, it now still writes
> `#define TUR_TD_<Name>` ahead of a non-parametric layout, so the pre-pass
> guard sees it -- the first fix direction, one line per such ADT.  All 155
> snapshots carrying a `TUR_FWD_tur_adt_*` forward decl are clean under
> `clang -std=c99 -fsyntax-only -Wtypedef-redefinition`; 16 snapshots moved,
> each by that one line.


**Severity: low** (a `-Wtypedef-redefinition` warning under `clang -std=c99`,
the flags `tur build` passes; a hard error for a strict-C99 compiler; silent
under gcc). Found 2026-09-19 while fixing the parametric twin of this in
`emit_registered_adt_app_rec` (the M7 branch): pre-existing on `main`, and
reproduced from `main`'s own snapshot.

## Repro

`tests/fixtures/byvalue-option-over-parametric-monomorph/expected.c` (on
`main`, before this branch):

```sh
clang -std=c99 -fsyntax-only tests/fixtures/byvalue-option-over-parametric-monomorph/expected.c
# warning: redefinition of typedef 'tur_adt_P' is a C11 feature [-Wtypedef-redefinition]
```

The emitted C, in order:

```c
typedef struct tur_adt_P {          /* the user ADT's full layout ... */
    int64_t x;
    int64_t y;
} tur_adt_P;                        /* ... with NO TUR_TD_tur_adt_P guard around it */
typedef tur_adt_P P;
...
#if !defined(TUR_FWD_tur_adt_P) && !defined(TUR_TD_tur_adt_P)
#define TUR_FWD_tur_adt_P
typedef struct tur_adt_P tur_adt_P;  /* re-typedefs the name: C11-only */
#endif
#ifndef TUR_TY_tur_adt_Option__P
```

The second block is the registered-app emitter's dependency pre-pass
(`types.c`, "structdef-retirement slice 1"): a non-parametric by-value
record held as `tur_adt_P *` inside `(Option P)`'s monomorph gets a forward
`typedef struct X X;`, guarded on `TUR_FWD_` **and** on `TUR_TD_`, the
full layout's macro, so that the two never both fire. But the full layout
above was emitted by a path that does not define `TUR_TD_tur_adt_P` (the
guarded emitters are `emit_module.c:9376` and `:16061`; this one is a
third), so the guard sees neither macro and re-typedefs a name that is
already a typedef.

Only ~1 in 40 `main` snapshots carries it (the shape needs a user ADT held
behind `(Option P)` / `(Result P E)`), and nothing in CI compiles a fixture
with clang under `-std=c99` except the `tur_offtree_load` probe on macOS,
which happens not to hit this shape -- which is why it has been silent.

## Fix direction

Either of:

- Make the third full-layout emitter define `TUR_TD_<Name>` like the other
  two (find it by the unguarded `typedef struct tur_adt_%s {` that precedes
  `typedef tur_adt_%s %s;`), so the pre-pass guard sees it; or
- Have that emitter introduce the name the way the parametric monomorph
  path now does: a guarded `typedef struct X X;` once (`TUR_FWD_X`), then
  `struct X { ... };` with the tag only -- then order no longer matters and
  no path can re-typedef the name.

The second is the one the parametric path took on 2026-09-19 (see
`types.c` `emit_registered_adt_app_rec`, "the typedef NAME is introduced
exactly once"); doing the same here retires the `TUR_TD_`-vs-`TUR_FWD_`
coordination entirely.
