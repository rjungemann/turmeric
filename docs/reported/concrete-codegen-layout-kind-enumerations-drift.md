# Three hand-maintained TypeKind enumerations must agree, and two of them drift

**Severity:** high. One missing arm in the first of these produced the
just-fixed silent-wrong-output bug in
[map-show-keyword-key-raw-int](../archive/map-show-keyword-key-raw-int.md). The
second is worse: ordinary code reaches a two-types-one-C-name collision that
routes a closure handle through a `double`, which round-trips exactly only while
addresses stay under 2^53. It compiles, it prints correct answers today, and it
is silently address-layout-dependent. See
[Reachability](#reachability-reached-by-ordinary-code-and-currently-masked).

**Status:** Finding 1 **FIXED 2026-07-29**; Finding 2 still open. See
[Resolution](#resolution-2026-07-29--finding-1).

## The shape of the problem

Three functions in `src/compiler/types.c` each switch over `TypeKind`, and
codegen is only correct when all three agree:

| function | question it answers | unlisted kind falls to |
| --- | --- | --- |
| `type_c_name` | what is this type's C spelling? | **no default arm** -- every kind is listed |
| `type_has_concrete_codegen_layout` | may this be an ADT type argument / spec type? | `default: return false` |
| `append_type_mangle` | what token names it inside a monomorph's C name? | `default: "opaque"` |

The first is exhaustive. The other two are not, and their fallbacks fail in
opposite directions:

- **`type_has_concrete_codegen_layout` fails closed.** A missing kind silently
  loses the by-value monomorph, and the value falls back to the int64 carrier.
  That is exactly the map-show bug: `TY_SYM` was absent, so
  `adt_app_is_byvalue_product((Vec Sym))` was false, `type_c_name`'s `TY_APP`
  arm fell through to `return "int64_t"`, no `(Vec Sym)` monomorph or
  constrained-instance specialization was emitted, and generic collection show
  bound `Show[int]` for the element -- printing the raw carrier integer. No
  diagnostic anywhere.
- **`append_type_mangle` fails open.** A missing kind is not dropped, it is
  *merged*: every unlisted kind mangles to the single token `opaque`, so two
  distinct instantiations can claim one C name.

## Finding 1 -- 9 of 33 concrete kinds share one mangling token

Mechanically extracted from the two switches (script in the session; re-derivable
by diffing their `case` labels):

```
CONCRETE but mangles to the shared "opaque" token:
  TY_CLONEABLE_CONT  TY_CONT  TY_EXCEPTION  TY_FN  TY_GENERATOR
  TY_HANDLER  TY_ROLE  TY_SESSION  TY_SET
                                              -- 9 of 33 concrete kinds
```

Every one of those is accepted as an ADT type argument *and* named `opaque`. Two
of them in the same translation unit collide. Demonstrated -- two `Box`
instantiations over two different function types:

```turmeric
(defdata Box [a] (MkBox a))
(defn takes-int-fn-box [b : (Box (fn [int] int))]     : int 0)
(defn takes-flt-fn-box [b : (Box (fn [float] float))] : int 0)
```

`tur emit-c` produces **two typedefs under one name**, with different layouts:

```c
typedef struct tur_adt_Box__opaque {
    union { struct { int64_t _0; } MkBox; } as;      /* (Box (fn [int] int))   */
} tur_adt_Box__opaque;
typedef struct tur_adt_Box__opaque {
    union { struct { double  _0; } MkBox; } as;      /* (Box (fn [float] float)) */
} tur_adt_Box__opaque;
```

and **two constructors under one name**, with different signatures:

```c
static tur_adt_Box__opaque ctor_MkBox__opaque(double  _0) { ... }
static tur_adt_Box__opaque ctor_MkBox__opaque(int64_t _0) { ... }
```

Both pairs are wrapped in `#ifndef TUR_TY_tur_adt_Box__opaque` /
`#ifndef TUR_FN_...` guards, so the **second of each is preprocessed away**. The
surviving definition is whichever the emitter reached first, and the other type
silently adopts its layout and its constructor. `int64_t` against `double` is not
a benign difference -- it is a different register class, the same hazard
`TUR-E0707` exists to reject elsewhere.

`Vec` masks this particular pair (its header is `void *data; int64_t len, cap`,
so both element types give an identical layout), which is why it took a
by-value product to see it. The *specializations* keyed on the mangled name
still collide even for `Vec`.

Note `(Set int)` is **not** affected: it is `TY_APP(Set, int)`, and the `TY_APP`
arm recurses properly (`tur_adt_Vec__Set__int`). Only a *bare* `TY_SET` reaches
the default arm.

## Reachability: reached by ordinary code, and currently masked

An earlier revision of this report concluded "real defect, no reachable trigger."
**That was wrong**, and the way it was wrong is instructive: the first probe used
a `(fn [] float)` payload, which fails to compile for an unrelated reason
([fn-payload-in-container-undeclared-temp](fn-payload-in-container-undeclared-temp.md)
-- niladic float thunks only). Narrowing *that* bug freed up fn types that do
compile, and the collision is reachable with them.

Two `Box` instantiations, each individually fine:

```turmeric
(defdata Box [a] (MkBox a))
(defn use-half [b : (Box (fn [int] float))] : float (match b (MkBox f) (f 15)))
(defn use-inc  [b : (Box (fn [int] int))]   : int   (match b (MkBox f) (f 41)))
```

This **compiles and prints the right answers** (`7.500000`, `42.000000`). The
emitted C shows why that is luck rather than correctness:

```c
static tur_adt_Box__opaque ctor_MkBox__opaque(double  _0) { ... }   /* survives */
static tur_adt_Box__opaque ctor_MkBox__opaque(int64_t _0) { ... }   /* #ifndef'd away */
...
__auto_type __ps_166 = (ctor_MkBox__opaque((int64_t)(intptr_t)(__t165)));
__auto_type __ps_172 = (ctor_MkBox__opaque((int64_t)(intptr_t)(__t171)));
```

Both call sites pass an `(int64_t)`-cast **closure pointer** into the surviving
constructor, whose parameter and field are `double`. So every boxed closure
handle in the colliding group makes an int64 -> double -> int64 round trip.

Measured threshold, with the exact surviving pattern extracted to standalone C:

```
0x55f1a2b3c4d5   -> 0x55f1a2b3c4d5   ok        (typical heap address, ~2^47)
0x10000000000000 -> 0x10000000000000 ok        (2^52)
0x20000000000001 -> 0x20000000000000 CORRUPTED (2^53+1 -- low bit lost)
0x7ffff2345671   -> 0x7ffff2345671   ok        (typical stack address)
```

A `double` holds 53 bits of mantissa. Ordinary Linux heap and stack addresses sit
around 2^47, comfortably inside it -- which is the entire reason the repro above
prints correct output. Anything that pushes a handle above 2^53 (a different
allocator, a platform with a higher mapping base, pointer tagging, a handle that
is not an address at all) corrupts it silently, with no diagnostic.

So the accurate statement is: **the collision is live, reachable from ordinary
code with no unsafe constructs, and currently masked by address layout.** Not a
latent hazard awaiting a new feature.

`TY_ANY` remains untestable from this angle: `(Vec any)` type-checks but cannot be
built, because `vec-of`'s type-witness binding trips `TUR-E0201` (`any` is treated
as unique) inside `stdlib/vec.tur`.


## Resolution (2026-07-29) -- Finding 1

`append_type_mangle` no longer has a `default` arm. All 60 `TypeKind` members now
have an explicit case, so `-Wall`/`-Wswitch` makes a newly added kind a build
failure here instead of a silent merge -- the same discipline that already keeps
`type_c_name` exhaustive.

**One token per kind was not enough.** The first attempt gave the nine
collision-class kinds distinct tokens (`fn`, `set`, `cont`, ...) and the
collision simply moved: `(Box (fn [int] float))` and `(Box (fn [int] int))` both
mangled `fn`. The real requirement is that the mangling be **injective with
respect to `type_eq`**, since `type_register_adt_app` keys its registry on
`type_eq`. So each arm now appends exactly what `type_eq` compares:

| kind | mangled payload | `type_eq` compares |
| --- | --- | --- |
| `TY_FN` | arity, arg kinds, result kind | same |
| `TY_PTR_VOID` | inner (recursive) | same |
| `TY_REF` / `TY_LREF` | `ref.inner` | same |
| `TY_RC` / `TY_WEAK` | `rc.inner` | same |
| `TY_REF_IMMUT` / `TY_REF_MUT` | `ref_borrow.target` | same |
| `TY_CONT` / `TY_CLONEABLE_CONT` | `cont.returns` | same |
| `TY_EXCEPTION` | `exn.payload_type` | same |
| `TY_HANDLER` | value + result kind | also `handled_row` (see below) |
| `TY_SET` | -- | nothing; one token is injective |

That the reference family needed this too was **not** in the original report:
`ref`, `lref`, `rc`, `weak`, `ref_immut`, `ref_mut` and `ptr_void` all had
pre-existing tokens that dropped their inner type, so `(Box rc<int>)` and
`(Box rc<float>)` were already colliding through arms that looked fine. Verified
fixed: they now mangle `tur_adt_Box__rc_int` and `tur_adt_Box__rc_float`.

Verified on the report's own repro: `tur_adt_Box__fn1_int__float` and
`tur_adt_Box__fn1_int__int`, one typedef each, constructors with their correct
`double` / `int64_t` signatures, output still `7.500000` / `42.000000`.

**Known residual.** `TY_HANDLER`'s `type_eq` also consults `handled_row`, which
has no short spelling here, so two handler types differing *only* in their
handled row still merge. Narrower than before and recorded rather than silent.

### Guard

`tests/check-typekind-mangle-exhaustive.sh` (ctest `tur_typekind_mangle_tests`)
checks all three properties from the source text, no build required: no `default`
arm, a case for every enum member (60), and no two kinds sharing a bare token (44
distinct). Verified to gate on both failure modes -- reintroducing a `default`
arm and pointing two kinds at one token each fail it.

### Cost

140 codegen snapshots regenerated, all one cause: `opaque` -> `fn1_int__int`,
1820 sites. No other token moved in any fixture.

Two fixtures went red, and they are a **pre-existing defect made visible**, not a
regression -- filed as
[ap-spec-records-wrong-fn-element-type](ap-spec-records-wrong-fn-element-type.md).
An `ap` specialization records the function-element argument type from a
*different* call site; while both fn types mangled `opaque` the declared parameter
and the passed variable agreed as C types, so nothing could complain. Carried red
deliberately: a hard `cc` error beats a merged C name, and reverting would
restore the collision.

## Finding 2 -- which absent kinds are correctly absent

Not every kind missing from `type_has_concrete_codegen_layout` is a bug. Audited
against what `type_c_name` actually returns:

**Correctly rejected -- no real C type.** `type_c_name` gives these a
comment-void placeholder, i.e. they are type-level only and never a runtime
value: the session-protocol family (`TY_SEND`, `TY_RECV`, `TY_CLOSE`,
`TY_CHOOSE`, `TY_BRANCH`, `TY_SESSION_REC`, `TY_TIMEOUT`, `TY_SESSION_PAIR`,
`TY_SESSION_RECV_PAIR`, `TY_SESSION_OFFER`, `TY_GLOBAL`), plus `TY_TYPECLASS`,
`TY_TYPECLASS_INST`, `TY_TYPEROW`, `TY_DYNVAR`, `TY_FORALL`, `TY_EXISTS`,
`TY_NEVER`. `TY_STRUCT` is dead outright (structdef-retirement: no `Type` ever
has that kind).

**Questionable -- a real C type, but rejected.** These deserve the same
treatment `TY_SYM` just got, or a comment saying why not:

| kind | `type_c_name` | note |
| --- | --- | --- |
| `TY_ANY` | `tur_tagged_t` | a real two-word struct, and a first-class gradual-typing value |
| `TY_UNION` | `tur_tagged_t` | same representation as `any` |
| `TY_REC` | `int64_t` | recursive-type handle |
| `TY_INTERSECTION` | `int64_t` | documented as an IT2 placeholder |
| `TY_CONTRACT` | delegates to its base type | so concreteness should delegate too |

`TY_ANY` / `TY_UNION` are the interesting pair: `tur_tagged_t` is **two words**,
so if either is ever admitted to the concrete list while still mangling to
`opaque`, it can collide with a one-word kind from Finding 1 and the layouts will
differ in *size*, not just register class. Admitting them without fixing the
mangling first would upgrade a latent defect into a live one.

## Fix directions

Roughly increasing cost:

1. **Give `append_type_mangle` a real token per kind, or make its default
   loud.** The cheap, high-value half -- and given the Reachability section, the
   urgent one. Every kind in the Finding 1 list needs a
   distinct token (`fn`, `set`, `cont`, `handler`, ...). Better still, delete the
   `default` arm so the compiler's `-Wswitch` catches the next new kind at the
   point it is added -- the same discipline that keeps `type_c_name` exhaustive.
   This is the change that makes Finding 1 unreachable-by-construction rather
   than unreachable-by-accident.
2. **Audit the five questionable kinds** in Finding 2, adding each to the
   concrete list or a one-line comment saying why it is excluded. Do this
   *after* (1), per the size-mismatch note above.
3. **Collapse the three enumerations into one table.** The root problem is that
   three switches encode overlapping facts about the same kind and nothing
   enforces agreement. A single `KIND_INFO[]` row per kind
   (`c_name`, `mangle_token`, `is_concrete`) read by all three would make drift a
   compile error instead of a silent wrong-output bug. This is what would have
   prevented the map-show bug, and it is the only direction that prevents the
   next one.

## Coverage to add with the fix

There is no test anywhere that asserts these enumerations agree. A cheap
guard, in the spirit of `tests/check-span-coverage.sh`: for every `TypeKind`,
assert that if `type_has_concrete_codegen_layout` accepts it then
`append_type_mangle` gives it a token other than `opaque`. That single assertion
covers all of Finding 1 and fails loudly the next time a kind is added to one
switch and not the other.
