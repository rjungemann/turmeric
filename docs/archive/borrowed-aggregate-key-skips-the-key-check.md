# A borrowed aggregate argument is never checked against a `(& K)` parameter

**RESOLVED 2026-09-26** by the first fix direction below: a borrow type now
records its target's full type. `ref_borrow.target_full`
(src/compiler/types.h) is set for a struct, ADT or applied target -- by
`elab_borrow_immut` / `elab_borrow_mut` from the borrowed expression, and by
the `(& T)` type form from `T` -- and left NULL where the kind already names
the type (a scalar) or `target_tyvar` does (a tyvar). Then:

- `type_eq` on two borrows compares recorded targets when both have one, and
  falls back to the kind otherwise (an imported signature, an owning-ref
  reborrow), so nothing that compared equal by kind alone and has no target
  changes.
- The H9 arm of `call_collect_type_bindings` binds / compares K through a
  borrowed aggregate's full type instead of skipping it, and `(& (Vec A))`
  binds A from `&(Vec int)`. Both repros are refused -- `expected &int, got
  &String` and `expected &int, got &Pt` -- and the diagnostic prints the
  target, not `&adt`.
- A concrete `(& Pt)` parameter compares its target too. It used to take a
  borrow of ANY ADT (`(getx (& q))` for a `Qt`), a second face of the same
  hole found while fixing this one.
- In a Saffron file the K = `any` refusal now happens, so the borrow seam
  sees a struct key and passes `&TUR_TAG(...)` -- a box -- where it used to
  pass `&p`.
- `(deref p)` of a borrow is the recorded target (`&Pt` derefs to a `Pt`, so
  `(.px (deref p))` resolves -- it used to be a bare ADT kind, "no typeclass
  method found for 'px'"), and `(deref k)` of a `(& K)` is the NAMED tyvar,
  so the emitter resolves it through the spec: at K = `any` it loads the
  whole `tur_tagged_t` rather than one `int64_t` of it. Without that, the
  Saffron repro below would have been fixed at the call and broken in the
  callee.

Pinned by `errors/borrowed-aggregate-key-refused`,
`errors/borrowed-aggregate-param-refused`, `borrowed-aggregate-target-typed`
(the accepting side: matching aggregate keys, `(& (Vec A))`, field reads
through a borrow) and `saffron-borrowed-struct-key-widened` (a `(& K)` callee
that reads its key: `(& p)`, `(& "s")` and `(& 4.25)`, both back ends).

The original report follows.

**Severity: medium.** A typed map accepts a key of the wrong type, silently, on
both back ends -- no diagnostic, and the map then holds keys of two types. Not
Saffron-specific: the repro is plain typed Turmeric. Pre-existing: the code path
(below) predates the branch that filed this, which only touched the scalar case.

Found 2026-09-26 while filing what was thought to be a leak in the Saffron key
seam (saffron-open-generic-result-not-grounded). It was not a leak -- measured
under LeakSanitizer, a struct key through the seam leaks nothing per call, only
the probe's own never-freed map -- but measuring it turned up this.

## Repro

```turmeric
(load "stdlib/string.tur")
(defn main [] : int
  (let [m (map-assoc (:: (map-new) (Map int int)) 1 10)
        k (string/from-cstr "a")]
    (println (map-has? m k))
    (println (map-count (map-assoc m k 20))))
  0)
```

```
$ tur check r.tur ; echo $?
0
$ tur run r.tur
false
2
$ tur --interpret r.tur
false
2
```

A `String` key went into a `(Map int int)`. The scalar twin is refused, as it
should be -- a `cstr` key reports `function 'tur-map-kcheck' arg 2: expected
&int, got &cstr`. The minimal form, with no map macro in the way:

```turmeric
(defdata Pt (Pt [px : int py : int]))
(defn kr [K V] [m : (Map K V) k : (& K)] : (Map K V) m)
(defn main [] : int
  (let [m (:: (map-new) (Map int int))
        p (Pt 1 2)
        s "str"]
    (println (map-count (kr m (& p))))    ; accepted -- K is int, p is a Pt
    (println (map-count (kr m (& s)))))   ; refused: expected &int, got &cstr
  0)
```

## Root cause

A borrow type carries its target as a bare `TypeKind`
(`ref_borrow.target`, src/compiler/types.h:815; built from `inner->type.kind`
in `elab_borrow_immut`, src/compiler/elab_structs.c:5548), so `&Pt`, `&String`
and `&(Vec int)` are all just "a borrow of an ADT/app".

The borrow arm of `call_collect_type_bindings`
(src/compiler/elab_call.c:1252, saffron-dynamic-surface-pass H9) binds K from a
borrowed argument only when that kind fully names a type -- a scalar. For
anything else it returns true without binding and without comparing, on
purpose: "A struct / String / applied key (`&<adt>`) must not bind K from its
kind -- it would clash with the K the map argument already bound with its full
type". That is right as far as BINDING goes, but nothing then CHECKS the
argument against the K the map bound, so the call is accepted. The argument
check itself compares only the parameter's kind (`TY_REF_IMMUT`).

## Also reached from Saffron

Two consequences in a dynamic file:

- The same hole: a program-typed `(:: (map-new) (Map Sym int))` takes a
  `String` key. `errors/saffron-typed-map-rejects-wrong-key` pins only the
  scalar refusal.
- A struct key into a `(Map any any)` is accepted by this path before the
  Saffron key seam (elab_call.c, "the BORROW twin of the widen seam") is
  consulted, so the seam never widens it: the callee's `&any` receives `&p`,
  the address of a raw `Pt`, where it expects a tagged box. Latent today --
  `tur-map-kcheck` is the only `(& K)` callee in the tree and it never reads
  its key -- but a callee that dereferenced it would misread. The seam's
  scalar path is correct (`&TUR_TAG(...)`).

## Fix directions

- Give the borrow type its target's full type, not only its kind (a `Type *`
  beside `ref_borrow.target`, filled in `elab_borrow_immut` from the inner
  expression). Then the H9 arm can compare a borrowed aggregate against an
  already-bound K with `type_eq`, and refuse a mismatch the way the scalar
  arm already does.
- Cheaper, local: at the argument check, when the parameter is `(& K)` with K
  bound and the argument is an `EX_BORROW_IMMUT`, compare the borrowed
  expression's own full type (`args[i]->as.borrow_immut_.expr->type`) with
  the binding.
- Either way, let the Saffron key seam see aggregate keys when K is `any`, so
  they are widened (through the ascription path a hand-written `(& (:: p
  any))` takes, which emits a correct box) rather than passed raw.
- Pin both: the typed refusal (String and struct keys into a `(Map int
  int)`), and a Saffron struct key reaching a `(& K)` callee that reads it.
