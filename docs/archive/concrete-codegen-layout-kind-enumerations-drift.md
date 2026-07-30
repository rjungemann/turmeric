# Three hand-maintained TypeKind enumerations must agree, and two of them drift

**Severity:** high. One missing arm in the first of these produced the
just-fixed silent-wrong-output bug in
[map-show-keyword-key-raw-int](map-show-keyword-key-raw-int.md). The
second is worse: ordinary code reaches a two-types-one-C-name collision that
routes a closure handle through a `double`, which round-trips exactly only while
addresses stay under 2^53. It compiles, it prints correct answers today, and it
is silently address-layout-dependent. See
[Reachability](#reachability-reached-by-ordinary-code-and-currently-masked).

**Status:** **RESOLVED.** Finding 1 fixed 2026-07-29
([Resolution](#resolution-2026-07-29--finding-1)); Finding 2 fixed 2026-07-30
([Resolution](#resolution-2026-07-30--finding-2)).

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
([fn-payload-in-container-undeclared-temp](../reported/fn-payload-in-container-undeclared-temp.md)
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
[fn-element-tyvars-not-substituted-in-spec-types](fn-element-tyvars-not-substituted-in-spec-types.md).
A function-typed element's tyvars are never substituted into a spec's argument
types -- `emit_abi_instantiate_type` has no `TY_FN` arm, and a fn keeps its
tyvars only in the out-of-line `arg_full_types`/`result_full_type`. While both
fn types mangled `opaque` the declared parameter and the passed variable agreed
as C types, so nothing could complain. Carried red
deliberately: a hard `cc` error beats a merged C name, and reverting would
restore the collision. **Both were fixed on 2026-07-30** -- `run.sh` is now
2417/0.

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

## Resolution (2026-07-30) -- Finding 2

Three changes, in `src/compiler/types.c`. The audit came back with one
admission, one *new* live collision, and eight documented exclusions.

### The correction to Finding 2's premise

The report says the questionable kinds are "rejected", implying they never
reach a monomorph name. **They do.** `type_register_adt_app` rejects only
`TY_TYVAR` / `TY_UNKNOWN` type arguments (types.c:1155); every other kind gets a
registry entry and a `tur_adt_<Name>__<token>` typedef *regardless* of what
`type_has_concrete_codegen_layout` says. The concrete list controls whether the
value flows by value and whether constrained instances specialize -- not whether
a name is minted. So the Finding 1 rule ("the mangling must be injective with
respect to `type_eq`") applies to **every** kind, including the ones this switch
rejects, and the "they are rejected anyway" hedge in the old mangler comment was
not load-bearing after all.

Under that rule, eight kinds were still merging, because their arms spelled a
bare token while `type_eq` discriminates on a payload:

| kind | `type_eq` compares | now mangles |
| --- | --- | --- |
| `TY_UNION` | members, structurally | `union2_int_float` |
| `TY_INTERSECTION` | members, structurally | `intersection2_...` |
| `TY_REC` | `rec.name` (interned) | `rec_<name>` |
| `TY_TYPEROW` | elements + field names, order-sensitive | `typerow2_<e0>_<e1>` |
| `TY_FORALL` / `TY_EXISTS` | n_vars + constraint set + body | `forall1_0__int` |
| `TY_TYPECLASS` | the `TypeClass *` | `typeclass_<name>` |
| `TY_TYPECLASS_INST` | the instance pointer | `typeclass_inst_<class>_<args>` |
| `TY_CONTRACT` | -- see below | `contract_<base>` |

Verified live for unions before the fix: `(Box (| int float))` and
`(Box (| int cstr))` both emit `tur_adt_Box__union`, two typedefs under one
name, second `#ifndef`'d away. Both bodies carry a `tur_tagged_t` field, so
this one is layout-safe by luck -- but every name-keyed specialization
downstream still merges, which is the map-show failure mode.

### The one that was not layout-safe: `TY_CONTRACT`

`TY_CONTRACT` failed in *both* directions at once, and this is the live defect
Finding 2 turned up:

- `type_c_name` **delegates to the base type**, so `{ x : int | .. }` is
  `int64_t` and `{ y : float | .. }` is `double` -- two different layouts.
- `type_eq` had **no arm at all** for it, falling through to the trailing
  `return 1`: *every* pair of contract types compared equal.

So the two shared one registry entry and one `tur_adt_Box__contract` typedef.
Emitted before the fix, from `(Box { x : int | (> x 0) })` alongside
`(Box { y : float | (> y 0.0) })`:

```c
typedef struct tur_adt_Box__contract {          /* survives */
    union { struct { int64_t _0; } MkBox; } as;
} tur_adt_Box__contract;
typedef struct tur_adt_Box__contract {          /* #ifndef'd away */
    union { struct { double  _0; } MkBox; } as;
} tur_adt_Box__contract;
...
double v_1307 = (double)__scrut->as.MkBox._0;   /* float arm reads the int64 field */
```

Note the read is a *numeric* `(double)` conversion off an `int64_t` field --
worse than Finding 1's bit-preserving round trip, and not masked by address
layout. Mangling alone could not fix it: with `type_eq` saying the two types
are equal, the registry never creates a second entry to name differently. So
`type_eq` gained a `TY_CONTRACT` arm comparing base types. Predicates are
deliberately *not* compared -- they are run-time-checked and never C-visible,
so `{ x : int | (> x 0) }` and `{ x : int | (< x 0) }` stay interchangeable
exactly as before.

With identity fixed, the report's own recommendation for this kind follows:
`type_has_concrete_codegen_layout` now **delegates to the base type** too, so
`(Box { y : float | .. })` monomorphises with the `double` field its base asks
for instead of losing the by-value monomorph to the int64 carrier. Result:
`tur_adt_Box__contract_int` (int64 field) and `tur_adt_Box__contract_float`
(double field), one typedef each, constructors with matching signatures.

### The other four questionable kinds -- excluded, with reasons

Each now carries its rejection in the switch rather than falling off a
`default`:

- **`TY_ANY` / `TY_UNION`.** `tur_tagged_t` is a real C type and it is **two
  words**; every kind on the accepted list is one. Admitting them is a
  by-value-ABI change (a 16-byte monomorph field with its own copy/drop
  crossings), not a table edit, and there is still no way to build one to test
  it -- `(Vec any)` type-checks but `vec-of`'s type-witness binding trips
  `TUR-E0201` inside `stdlib/vec.tur`. Their tokens are now distinct, so the
  size-mismatch hazard the report warned about ("admitting them without fixing
  the mangling first would upgrade a latent defect into a live one") is closed
  ahead of any future admission.
- **`TY_REC` / `TY_INTERSECTION`.** `type_c_name` gives both the plain
  `int64_t` carrier, so a by-value monomorph over either would have exactly the
  carrier's layout and buy nothing but an extra C name. Revisit if either grows
  a representation of its own (`TY_INTERSECTION` is documented as an IT2
  placeholder).

The kinds the report classed as correctly rejected keep that verdict, with one
refinement: `TY_SESSION_PAIR` / `TY_SESSION_RECV_PAIR` / `TY_SESSION_OFFER` are
**not** comment-void (they lower to `void *` / `int64_t`), so they are excluded
on a different ground -- they are internal result types of `make-session` /
`recv` / `offer` that exist only between the call and its destructuring pattern,
and are never written by a user in a type-argument slot.

### Discipline: the third switch is exhaustive now too

`type_has_concrete_codegen_layout` has **no `default` arm**. All 60 `TypeKind`
members have an explicit case, so `-Wall`/`-Wswitch` makes a newly added kind a
build failure there as well -- which is fix direction 3's actual goal (drift
becomes a compile error) reached without collapsing three switches whose arms
are mostly *computed*, not table lookups: `type_c_name` registers fn-pointer
typedefs and consults `AdtDef`s, and the mangler now recurses into payloads. A
`KIND_INFO[]` row could carry only the bare-token minority; the switches would
survive anyway, and a table that covers a third of the cases enforces nothing.
Two of the three enumerations were unenforced when this report was filed; zero
are now.

### Guard

- `tests/check-typekind-mangle-exhaustive.sh` gained two properties (now five):
  every payload-comparing kind still mangles a payload (a kind cannot quietly
  decay back to a bare token), and `type_has_concrete_codegen_layout` has no
  `default` arm and a case per enum member. Verified to gate on both.
- `tests/check-monomorph-name-collision.sh` (new; ctest
  `tur_monomorph_name_collision`) is the behavioural half -- it compiles four
  programs that instantiate one ADT at two type arguments (fn payloads, `rc`
  inners, union members, contract bases) and asserts (a) no `#ifndef`-guarded
  name is defined twice with differing bodies, and (b) the two expected
  monomorph names both appear. Property (b) is what catches a merge whose two
  layouts coincide, like the union pair. Verified to gate: reverting the union
  and contract mangle arms fails it, and the contract failure prints the
  `int64_t`-vs-`double` body divergence.

### Cost

**Zero fixture churn** -- `bash tests/run.sh` is 2436/0 both before and after.
No fixture instantiates a parametric ADT at a contract, union, intersection,
rec, row, quantified or typeclass type argument, which is also why the
collision survived this long.

### Not fixed here

A contract-typed payload is still hard to *use* at a boundary, for reasons
outside this report: `(match b (MkBox v) v)` returning `v` as `float` trips
`TUR-E0707` (the contract is not peeled to its base for the register-class
check), and `(println v)` finds no overload for `{ y : float | .. }`. The
layouts are right now; the surface still needs the peel.

## Fix directions

Roughly increasing cost. **(1) done 2026-07-29, (2) done 2026-07-30, (3)
superseded** -- see the two Resolution sections above; (3)'s goal (drift is a
compile error) is met by making all three switches exhaustive, without the
table.

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

**Added.** Both halves, and the assertion ended up stronger than proposed --
"a token other than `opaque`" is not enough, because a merge can also happen
between two *listed* kinds (Finding 1's `fn` vs `fn`) or within one kind whose
payload is dropped (Finding 2's `union` vs `union`). What the guards assert is
injectivity against `type_eq`, plus exhaustiveness of both switches:
`tests/check-typekind-mangle-exhaustive.sh` (five source-text properties, no
build) and `tests/check-monomorph-name-collision.sh` (four compiled repros).
