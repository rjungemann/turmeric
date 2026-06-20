---
title: HKT instance method (`fmap` over `Either`) consumed by a `match` scrutinee is called but never emitted -> undefined reference at link
category: Typeclass instance-method monomorphization / emission gap -- link-time defect (undefined symbol), not a miscompile
severity: Medium. Blocks any program that pattern-matches the *result* of a
  higher-kinded instance method (`(match (fmap e f) ...)`). The call site emits
  a direct call to the carrier-ABI instance-method clone
  `__inst_Functor_fmap_Either__ltstruct_gt`, but that specialization is never
  materialized, so cc reports `implicit declaration` + the linker reports
  `undefined reference`. Reproduces in plain single-file whole-program builds
  (`tur build <file>`) -- it is NOT separate-compilation-specific, despite the
  run-build-project.sh test framing. Workaround: route the result through a
  by-value consumer (`from-right`/`from-left`) instead of `match`.
status: OPEN
---

# HKT instance method feeding a `match` scrutinee dangles at link

## One-line summary

`(match (fmap e f) (Left l) l (Right r) r)` emits a direct call to
`__inst_Functor_fmap_Either__ltstruct_gt(...)` but no definition (or even a
forward decl) for that symbol is ever emitted, so the program fails to link.
The same `fmap` call consumed by a normal function call links fine.

## Severity / impact

This is exactly the kind of rough edge that makes the `frame`/spice agent throw
up its hands: `tur check` is clean, the code is idiomatic, and the failure only
appears as a wall of cc `implicit declaration` warnings followed by a linker
`undefined reference`. The trigger (consuming an HKT instance method's result
with `match`) is extremely common in real code.

## Minimal repro (single file -- NOT separate compilation)

```turmeric
;; t.tur
(load "stdlib/either.tur")
(defn inc [x : int] : int (+ x 1))
(defn main [] : int
  (match (fmap (Right 21) inc)
    (Left l)  l
    (Right r) r))
```

```sh
$ tur build t.tur -o /tmp/tb
t.tur.c: In function 'main':
t.tur.c:NNNN: warning: implicit declaration of function
  '__inst_Functor_fmap_Either__ltstruct_gt' [-Wimplicit-function-declaration]
/usr/bin/ld: ... undefined reference to `__inst_Functor_fmap_Either__ltstruct_gt'
collect2: error: ld returned 1 exit status
```

The emitted C contains exactly one occurrence of the symbol -- the call site,
with no matching definition:

```c
tur_adt_Either *__scrut = (tur_adt_Either *)(intptr_t)(
    __inst_Functor_fmap_Either__ltstruct_gt(
        e_1047, (tur_poly_fn_t){ NULL, (int64_t(*)(void*,int64_t))__poly_1051 }));
```

## What narrows the trigger

Bisected against `(load "stdlib/either.tur")` + the stdlib `Functor [(Either E)]`
instance (`stdlib/either.tur:176`):

| Program | Result |
| --- | --- |
| `(from-right -1 (fmap (Right 41) inc))` | **ok** |
| `(from-right -1 (fmap (Right 41) (fn [x : int] : int (* x 2))))` | **ok** |
| `(let [e (Right 21)] (from-right -1 (fmap e inc)))` | **ok** |
| `(match (fmap (Right 21) inc) (Left l) l (Right r) r)` | **FAIL** |
| `(let [e (Right 21)] (match (fmap e inc) (Left l) l (Right r) r))` | **FAIL** |

The single discriminator is **whether the `fmap` result is consumed by a
`match` scrutinee**. The lambda-vs-named-fn and let-bound-vs-direct container
axes do not matter. The existing passing fixture
`tests/fixtures/sum-either-functor-instance/` only ever feeds `fmap` into
`from-right`/`from-left`, which is why it has never caught this.

## Root cause (directional)

When the `fmap` result flows into a `match` scrutinee, the result is consumed
through the **carrier ABI** -- the `Either` ADT as a `tur_adt_Either *` pointer
(`(tur_adt_Either *)(intptr_t)(...)` in the snippet above). That forces the
carrier specialization of the instance method, whose suffix the mangler renders
as `__ltstruct_gt` (the legacy `<struct>` token -- see the adjacent open report
`instance-suffix-mangler-tyvar-element-legacy-struct-token.md`).

The call site (`elab_call.c` / `emit_call_name` -> `__inst_<Class>_<method>_<T>`,
see the note at `elab_typeclasses.c:4867`) resolves to that carrier-ABI clone
name, but the spec/clone-collection that materializes instance-method
specializations for emission (`emit_module.c`, around the
`emit_instance_is_live` / instance-method-spec machinery at lines 1135, 1935,
2933) never enqueues the `match`-scrutinee carrier specialization. By-value
consumers (`from-right`) request the already-emitted by-value clone, so they
link; the carrier clone is named-but-orphaned.

Net: the call-name resolution and the emission-set collection disagree about
which monomorphization a `match`-scrutinee consumer needs. The fix is to make
instance-method spec collection enqueue the carrier-ABI specialization whenever
a call site resolves to it (or to make the `match`-scrutinee consumer request
the by-value clone the producer actually emits).

## Where it bites in the suite

`tests/run-build-project.sh` carries two tests that fail on exactly this symbol:

- `build-project-load-higher-kinded-module`
- `build-project-import-higher-kinded`

Both were already RED on the base branch (confirmed by stashing all local
changes and rebuilding) -- they are pre-existing, not a regression. Their
in-script comments attribute the failure to separate-compilation per-module
emission, but the minimal repro above shows it is the whole-program path too;
the framing is misleading and the real fault is the instance-method
specialization collection, independent of compilation mode.

## Repro fixtures on disk

- Passing contrast: `tests/fixtures/sum-either-functor-instance/`
- Failing (project): `tests/run-build-project.sh` -> `LOADHK` / `IMPHK` sections.
