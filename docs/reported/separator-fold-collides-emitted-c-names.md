# Every separator folds to `_`, so distinct names share one emitted C symbol

**Severity: medium.** Most of this family is a loud cc error, and would be low
on its own. One arm is a **silent wrong answer with no diagnostic at any
layer** -- hand-written inline C calling `ctor_b_c` reaches a *different ADT's*
constructor -- and that arm is what sets the severity.

**Status:** OPEN. Filed 2026-09-02 while resolving
[duplicate-ctor-names-collide-in-emitted-c](../archive/duplicate-ctor-names-collide-in-emitted-c.md),
whose fix qualifies the constructor symbol with its ADT and explicitly leaves
this open. Pre-existing for the type-name arms; the silent arm arrived with
that fix (see **Attribution**).

## The shape

The legacy fold maps every non-alphanumeric character to `_`, and a literal `_`
is not re-encoded (`tur_mangle_legacy_append`, `mangle.c:129`). Emitted names
then *join* their parts with `_` or `__`:

| Emitted name | Built by | Joiner |
|---|---|---|
| `ctor_<Adt>_<Ctor>` | `mangle_ctor_symbol`, `emit_core.c:2037` | `_` |
| `tur_adt_<Adt>__<arg>__<arg>` | `append_adt_app_type_suffix`, `types.c:1674` | `__` |

A joiner that the fold can also *produce* is not a separator. Two source names
that differ only by where a separator falls therefore collide.

## Repros

All four reproduce against `336865d`. A/B/C are loud; D is not.

### A -- constructor symbol, two ways to split (loud)

```turmeric
(defdata a-b (c :int))
(defdata a   (b-c :int))
(defn main [] : int 0)
```

```
error: conflicting types for 'ctor_a_b_c'; have 'tur_adt_a(int64_t)'
```

Both spell `ctor_a_b_c`: ADT `a-b` + ctor `c`, and ADT `a` + ctor `b-c`.

### B -- an ADT named like a monomorph (loud)

```turmeric
(defdata Foo [a] (MkFoo a))
(defdata Foo__int (MkOther :int))
(defn use [] : int (match (MkFoo 7) (MkFoo v) v))
(defn main [] : int (println (use)) 0)
```

```
error: redefinition of 'struct tur_adt_Foo__int'
```

The user's ADT `Foo__int` and the monomorph of `(Foo int)` are the same C type
name. **Loud in both declaration orders** -- verified with the user ADT first
too; the user ADT's typedef is emitted unguarded, so the guarded monomorph
typedef still lands and collides rather than being silently skipped.

### C -- nested monomorph (loud, and it cannot be reached quietly)

`(Box (Pair2 int))` and `(Box Pair2__int)` both spell
`tur_adt_Box__Pair2__int`, and the emitter really does write that typedef twice
with different layouts. But reaching the outer collision requires instantiating
`(Pair2 int)`, which trips B at the base first. Worth recording as a
**negative result**: the outer arm has no quiet path of its own.

### D -- the silent one: bare ctor alias binds to the wrong ADT

````turmeric
; Both parametric, so both constructors lower to the int64 carrier and C's type
; system cannot tell the mis-binding from the real thing.
(defdata X [t] (XNil) (b-c t))     ; b-c is variant 1
(defdata Y [t] (b_c t) (YNil))     ; b_c is variant 0

(defn tag-of [h : int] : int
  ```c
  return (int64_t)((tur_adt_X *)(intptr_t)h)->tag;
  ```)

(defn make-y [n : int] : int
  ```c
  return ctor_b_c(n);
  ```)

(defn main [] : int
  (println (tag-of (make-y 42)))   ; asked for Y's b_c (0); prints 1
  0)
````

Builds with **no diagnostic of any kind** -- no turmeric error, no cc warning,
no ASan report -- and prints `1`. The inline C asked for `Y`'s `b_c` and got
`X`'s `b-c`.

Note what had to line up for this to be silent, because it is also the guide to
which shapes are dangerous: **both ADTs must lower to the same C type.** With
non-parametric ADTs the two constructors return `tur_adt_X` and `tur_adt_Y`, and
cc rejects the mismatch (`incompatible types when returning type 'tur_adt_X'`).
Parametric ADTs both lower to the `int64_t` carrier, so nothing downstream can
tell. The carrier is doing what it always does -- erasing the distinction the
check would need.

## Root cause

Two independent causes; D needs both.

**1. The joiner is inside the folded alphabet.** `_` and `__` separate parts of
a name whose parts can themselves contain `_` -- either written literally or
folded from `-` or `/`. Nothing escapes or length-delimits, so the split is
ambiguous. This is the whole of A, B and C.

**2. The alias census keys on the RAW name but guards on the MANGLED one**
(`emit_core.c`). `ctor_base_name_is_unique` (`:1977`) compares
`c->name` -- the source spelling -- so `b-c` and `b_c` are two distinct,
individually-unique names and both qualify for an alias. But
`emit_ctor_bare_alias` (`:1995`) emits under `#ifndef TUR_CTORALIAS_<mangled>`
(`:2001`), and both mangle to `b_c`. The first ADT emitted wins the macro; the
second's `#define` sits inside a now-false `#ifndef` and is silently dropped.
The emitted C shows both, which is what makes it easy to misread:

```c
static ... ctor_X_b_c(...) { ... }
#ifndef TUR_CTORALIAS_b_c
#define TUR_CTORALIAS_b_c
#define ctor_b_c ctor_X_b_c      /* <-- this one is live */
#endif
static ... ctor_Y_b_c(...) { ... }
#ifndef TUR_CTORALIAS_b_c        /* <-- already defined; body skipped */
#define TUR_CTORALIAS_b_c
#define ctor_b_c ctor_Y_b_c
#endif
```

A uniqueness test and its guard disagreeing about what "the same name" means is
the general defect; the fold is what makes them disagree.

## Attribution

Cause 1 is pre-existing and predates the constructor-qualification work; the
type-arg suffix convention has always had it.

**Cause 2 shipped with `336865d`** (the fix for
`duplicate-ctor-names-collide-in-emitted-c`), which added the bare-name
compatibility alias. That fix was explicitly designed to fail *closed* -- an
ambiguous constructor name gets no alias, so inline C naming it fails loudly
rather than binding to whichever ADT came first. It does that correctly for the
ambiguity it was checking (two ADTs owning the *same* name) and misses this one
(two ADTs owning names that *become* the same). The report noted the residual
fold ambiguity and did not connect it to the guard. That connection is the new
information here.

## Fix directions

In rough order of cost.

1. **Close cause 2 alone** (small, and it is the only silent arm). Make the
   census agree with the guard by keying `ctor_base_name_is_unique` on the
   MANGLED name -- `mangle_field_name(c->name)` -- rather than the raw one.
   `b-c` and `b_c` then read as one name owned by two ADTs, neither gets an
   alias, and inline C naming `ctor_b_c` fails at cc pointing at it. Restores
   the fail-closed property the alias was designed around. **Do this first
   whatever else happens**; it is a one-line key change plus a fixture, and it
   converts the only silent case in the family into a loud one.
2. **A collision diagnostic.** Detect two distinct source names folding to one
   emitted symbol and report it as a turmeric error naming both, instead of
   letting cc say `conflicting types for 'ctor_a_b_c'` with no hint that two
   ADTs are involved. Does not remove the restriction, but the current cc text
   gives a reader nothing to work from. Needs a per-program emitted-name map,
   which the ctor census is already most of.
3. **Make the scheme injective.** Length-prefix or escape the parts
   (`ctor_3Foo_5Bar_c`, or double a literal `_` when folding so the joiner is
   unambiguous). This is the real fix and it is wide: it moves every emitted
   constructor and monomorph type name, so ~150 `expected.c` snapshots and both
   seam canaries (`run-option-niche-seam.sh`, `run-sr4-seam.sh`) move with it,
   plus any hand-written inline C naming a monomorph. Worth coordinating as its
   own regen window rather than riding along with another change.

Note the ordering is deliberate: 1 removes the silent wrong answer for
essentially nothing, and 2 and 3 are then quality-of-diagnostic and
completeness work that can be scheduled rather than rushed.

## Workaround

Do not give two ADTs constructor names that differ only by a separator
(`b-c` vs `b_c`), and do not name an ADT after a monomorph spelling
(`Foo__int`). Neither is checked, and the first is not diagnosed at all.

## Guides to update when fixed

- [docs/guides/name-mangling-guide.md](../guides/name-mangling-guide.md) --
  its "ADT constructors" section states the residual ambiguity as a bounded
  caveat; it needs the silent arm called out until direction 1 lands, and
  rewriting entirely if direction 3 does.
