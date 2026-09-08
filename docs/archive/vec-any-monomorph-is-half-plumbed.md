---
title: A `(Vec any)` builds and runs, but its element type does not flow back out and its `vec-new` spec dedups against a sibling
category: Reported
description: RESOLVED. Both halves fixed. `vec-get` on a (Vec any) now reports each element's own type, and a `vec-of` producing a (Vec any) no longer emits -Wincompatible-pointer-types -- the cause was a MISSING vec-new monomorph one layer above the lookup-side fallback that reading the call sites kept pointing at.
---

# `(Vec any)` is buildable now, and half-plumbed

**RESOLVED 2026-09-08.** Both halves fixed; `tests/fixtures/vec-any-element-roundtrip`
and `tests/fixtures/vec-of-any-heterogeneous` pin them.

**Severity was medium.** Neither half was a miscompile -- the values stored were
correct and the element boxes were freed -- but the first made a `(Vec any)`
write-only, which is most of the point of having one, and the second meant only
one of the three routes to building one was shippable.

Residue of
[vec-of-any-repr-decision-ice](../archive/vec-of-any-repr-decision-ice.md),
which is fixed: `(Vec any)` no longer aborts the compiler. These are the two
things that fix did not reach, both found by measuring what the newly-buildable
type actually does.

## Half 1 -- `vec-get` on a `(Vec any)` returns `int` -- FIXED 2026-09-07 (S6)

**Fixed on the compiled path.** Two changes at the two ends of the same value:
`call_result_type`'s collapse of a bare-tyvar result to the int64 carrier now
exempts `any`/`union` (they are the two-word `tur_tagged_t`, so there is no
single word to reinterpret back from -- the reason that comment already gives
for the composites beside them); and the `any` readers bridge a carrier-form
operand back to the aggregate, since a boxed element arrives as the slot word.
`tests/fixtures/vec-any-element-roundtrip` pins int/cstr/float/bool each
reporting its own type.

The INTERPRETER now diverges instead, and worse -- it answers `bool` for all
four. Filed as
[vec-any-interp-keeps-one-element-tag](vec-any-interp-keeps-one-element-tag.md).

The original account follows.

### Original

```turmeric
(defn dyn [x : any] : any x)
(defn main [] : int
  (let [v (:: (vec-new) (Vec any))]
    (vec-push! v (dyn 7.1))
    (println (type-of (vec-get v 0))))
  0)
```

```
error: 'type-of' expects an 'any'-typed argument, got 'int'
```

Identical on both back ends, so this is elaboration, not codegen. The element
type does not flow out of the container: `vec-get [A] [v : (Vec A) i : int] : A`
should ground `A` to `any` from the receiver and does not.

Ascribing past it does NOT recover the element -- it makes it silently wrong,
which is the part worth knowing:

```turmeric
(println (type-of (:: (vec-get v 0) any)))   ; compiled: int   interpreted: float
```

That ascription is a WIDEN of an `int`-typed expression, so it re-tags the
carrier word with a statically-chosen tag rather than reading the element's own.
The two back ends pick different tags, which is how the shape announces itself.
The ascription is behaving correctly in isolation; what is wrong is the type it
is handed.

This is S6's subject (containers of `any` are that stage's whole point) and it
should be fixed there rather than as a local patch, since the same question --
"how does an element type reach a read through a container" -- governs
`#map{...}`, `#set{...}` and cons lists too.

## Half 2 -- a `vec-of` producing a `(Vec any)` does not clear the emitted-C ratchet -- FIXED 2026-09-08

**Fixed.** The cause was a MISSING SPEC, one layer above the lookup-side
fallback that reading the call sites kept pointing at. One disjunct in
`emit_abi_register_call`'s family-element rehydration (`src/compiler/emit_module.c`):

```c
if (ae2[k].kind == TY_TYVAR && ae2[k].as.tyvar_.name &&
    (type_has_concrete_codegen_layout(&ae1[k]) ||
     ae1[k].kind == TY_ANY ||                       /* <- added */
     (ae1[k].kind == TY_APP && type_app_is_concrete_adt(&ae1[k]))))
```

`any` is the THIRD kind of concrete element and answered no to both tests beside
it: the layout table rejects `TY_ANY` deliberately (a 16-byte by-value FIELD is
an ABI change -- a different question, and the row's own comment says so), and
`any` is not a `TY_APP`. So the `any` clone of `vec-empty-like__` synthesized no
`{A -> any}` binding, fell through the `n_bindings == 0` gate below, and never
interned its own `vec-new` monomorph. The cross-spec fallback then had only the
sibling `int` clone to find -- and took it.

`adt_app_type_arg_is_concrete` (types.c) already answers this same "concrete
enough to name a monomorph" question with the same three cases, `any` included;
it is `static` there, so the disjunct is repeated rather than shared.

`tests/fixtures/vec-of-any-heterogeneous` pins the shape S6/G7 needs -- a
heterogeneous `vec-of` where each element reads back with its own tag:

```
3
int
cstr
float
```

leak-clean under `tests/run-leak-check.sh`.

### What the original account got wrong, and how

Worth keeping, because the same wrong turn was taken three times.

**The warning was called cosmetic, twice.**

```
warning: returning 'tur_adt_Vec__int *' from a function with incompatible
         return type 'tur_adt_Vec__any *' [-Wincompatible-pointer-types]
```

`run.sh` greps every fixture's build stderr for exactly `-Wint-conversion` /
`-Wincompatible-pointer-types` and FAILs on a hit. That gate was sweep-verified
at ZERO across 2563 fixtures before it landed
(`docs/archive/emitted-c-pointer-integer-warnings-unwatched.md`), so a hit is a
regression signal, not noise -- and "the two monomorphs have identical layout,
so the pointer is the same pointer" was the wrong thing to conclude from. It is
a hard error under `-Werror`.

1. First recorded as "cosmetic" on a layout comparison alone.
2. Then narrowed to "only when a `(Vec any)` sits beside another `vec-of`",
   from manual `tur run` probes at `-O1`. `run.sh` builds at `-O2` and failed a
   SINGLE `vec-of` at `any`.

**And the cause was read off the call sites rather than measured.** The account
below built a detailed case against `emit_call_name`'s zero-argument
short-circuit, then against the cross-spec fallback beside it. Both are real
code and neither was the bug: the fallback took the sibling clone because there
was nothing else to take. Fixing the fallback would have masked the missing
spec.

The bisect that settled it took two probe rounds and is the method that worked
where four rounds of reading did not:

1. `ENTER` at the top of `emit_abi_register_call`, printing the active outer
   spec. Both clones enter.
2. `REACHED-INTERN` immediately before `emit_abi_intern_spec`. Only the `int`
   clone reaches it.
3. A `DECLINE <line>` probe on every `return` between the two -- **including
   the four written inline as `if (...) return;`**, which a first pass matching
   only bare `return;` lines missed, producing a run with no DECLINE line and
   no answer.

Round two printed:

```
ENTER vec-new outer=vec_empty_like____spec__tur_adt_Vec__int___int64_t
REACHED-INTERN vec-new outer=vec_empty_like____spec__tur_adt_Vec__int___int64_t
ENTER vec-new outer=vec_empty_like____spec__tur_adt_Vec__any___tur_tagged_t
DECLINE vec-new at B4278
```

one line naming the guard.

A third trap, worth its own line: the FIRST instrumented build **failed** (a
`%d` leaked into the probe's format string, `-Werror=format-extra-args`) and the
failure was not read, so the probe run used a stale binary and printed nothing.
"No output" from a probe means check that the build succeeded before it means
anything about the code.

### Original account

A `vec-of` that produces a `(Vec any)` emits:

```
warning: returning 'tur_adt_Vec__int *' from a function with incompatible
         return type 'tur_adt_Vec__any *' [-Wincompatible-pointer-types]
```

`run.sh` greps every fixture's build stderr for exactly
`-Wint-conversion` / `-Wincompatible-pointer-types` and FAILs on a hit. That
gate was sweep-verified at ZERO across 2563 fixtures before it landed
(`docs/archive/emitted-c-pointer-integer-warnings-unwatched.md`), so a hit is a
regression signal, not noise -- and "the two monomorphs have identical layout,
so the pointer is the same pointer" was the wrong thing to conclude from. It is
a hard error under `-Werror`.

I got this wrong in two stages, both by not asking the suite:

1. First recorded as "cosmetic" on a layout comparison alone.
2. Then narrowed to "only when a `(Vec any)` sits beside another `vec-of`",
   from manual `tur run` probes at `-O1`. `run.sh` builds at `-O2` and fails a
   SINGLE `vec-of` at `any`.

So only ONE route to a `(Vec any)` is currently shippable -- an explicit
`(:: (vec-new) (Vec any))`, which `tests/fixtures/vec-of-any-ascribed` pins.
The `vec-of` macro route builds and runs correctly but cannot be a fixture.

**This now BLOCKS S6's headline feature.** `[1 "two" 7.1]` in a Saffron file
lowers to `(vec-of ...)`, so G7 (a vector literal defaulting to `(Vec any)`)
cannot land while a single `vec-of` at `any` trips the emitted-C ratchet. It was
filed as a residue; it is on the critical path.

### Cause, as far as it is established

Inside `vec-empty-like__`'s `any` clone, the zero-argument `(vec-new)` call
resolves to `vec_new__spec__tur_adt_Vec__int__` even though
`vec_new__spec__tur_adt_Vec__any__` is emitted and is called correctly from
elsewhere in the same program. A probe on `emit_abi_intern_spec` confirms both
specs are CREATED (`SPEC-NEW vec-new result=(type-app Vec int)` and
`... (type-app Vec any)`), so the intern is not the problem -- the call site is.

`emit_call_name` (emit_core.c ~3482) short-circuits a zero-argument call
BEFORE the spec-matching loop:

```c
if (call->kind == EX_CALL &&
    (call->as.call_.n_args == 0 || (b && b->is_construct_template))) {
    ...
    return raw_name_for_binding(b);
}
```

**Established 2026-09-08, and it is not the short-circuit.** The call does not
reach it: it takes the exact-match path above and matches the WRONG entry.

That table is keyed on `(Expr*, active outer spec)`, which is right --
`vec-empty-like__`'s body is ONE Expr tree emitted once per clone, so a
per-`Expr*` key alone could hold only one answer. The defect is the CROSS-SPEC
FALLBACK beside it:

```c
if (ctx->specialized_call_outer[i] == active_outer) { matched = ...; break; }
/* Cross-spec fallback only inside a spec (active_outer != NULL) ... */
if (active_outer != NULL && !saw && !construct_into_carrier) {
    matched = ctx->specialized_call_names[i];
    saw = true;
}
```

Inside `vec_empty_like____spec__..._any` the `(vec-new)` Expr has an entry
recorded under the `int` clone's outer and NONE under the `any` clone's, so the
fallback takes the sibling -- a clone whose return ABI is `tur_adt_Vec__int *`
where this body returns `tur_adt_Vec__any *`. The comment directly above already
names that hazard for the NULL-outer case ("so it never routes a call to a
spec-scoped clone with a different return ABI") and does not guard the non-NULL
one.

### Narrowed further, 2026-09-08 -- four layers, each measured

Chasing "why is there no recording under the `any` outer" ruled out three
candidates and landed inside one function. Each step is a one-line `fprintf`,
run as `TUR_PROBE_REC=1 ./build/tur emit-c <file>` on
`(defn main [] : int (println (vec-len (vec-of (:: 2 any)))) 0)`:

1. **Not the recorder.** `emit_abi_record_specialized_call` keys on
   `(Expr*, active outer)` correctly, and IS called for the `any` clone of
   `vec-empty-like__` itself:
   `REC vec-empty-like clone=..._any_... outer=(NULL)`.
2. **Not the recursion guard.** The scan of a freshly-minted spec body
   (`if (fd && fd->body && ctx->n_abi_specializations != before_specs)`) RUNS
   for the `any` clone -- all three conditions true:
   `RECURSE? fd=1 body=1 newspecs=1 clone=..._any_...`.
3. **Not the intern.** The `int` clone's recursion reaches the intern site
   (`REACHED-INTERN vec-new result=(type-app Vec int) outer=..._int_...`); the
   `any` clone's produces NO such line.
4. **So the decline is upstream of the intern, inside `emit_abi_scan_expr`'s
   `EX_CALL` case**, and it is specific to `(Vec any)` -- `(Vec int)` walks the
   identical path in the same program and proceeds.

That is where the next probe goes: bisect the guards in that case for a
zero-argument callee whose result is a concrete app over `any`. The lookup-side
fallback below is then a SYMPTOM, and fixing it would only mask a missing spec.

The alternative fix -- **guard the cross-spec fallback on return ABI**, the way
`construct_into_carrier` guards it for a different reason -- stays available and
stays second choice: it treats the symptom, and declining the fallback drops the
call to the carrier base, which then has its own int64-vs-pointer straddle to
survive.

## Fix directions -- both taken

1. **Half 1, in S6**: ground a container read's element tyvar from the
   receiver's monomorph. Done at the two ends of the same value -- see Half 1.
2. **Half 2**: find why the per-`Expr*` recording that resolves a zero-argument
   call names the `int` clone inside the `any` clone's body. Done, and the
   answer was not on the lookup side at all: no recording existed under the
   `any` outer because no spec was ever interned for it.

The alternative -- **guard the cross-spec fallback on return ABI** -- was
correctly kept as second choice and is not needed. It treats the symptom, and
declining the fallback would have dropped the call to the carrier base, with its
own int64-vs-pointer straddle to survive.
