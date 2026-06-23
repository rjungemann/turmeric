---
title: Option's legacy `tur_option_t` special-casing not reconciled with #482 -- struct-field Option miscompiles in two codegen sites (typeclass-arg lowering + Result type ordering)
category: Carrier <-> Concrete ABI -- Option-specific residual special-casing vs #482 embedded-aggregate field layout
severity: Medium-high. Two codegen sites still assume the pre-#482
  "struct fields hold Options as a `tur_option_t *` heap box" representation,
  so an `(Option T)` struct field miscompiles while every other by-value
  parametric field type (`Pair`, `Cons`, user struct) works. Site 1 inserts a
  bogus `tur_option_t *` reconstruction round-trip (hard C error: "aggregate
  value used where an integer was expected"); Site 2 emits `Result__T` before
  `T` when `T` embeds an `Option` field (hard C error: "unknown type name 'T'"
  + ok_val type cascade). Blocks `derive-json` over structs with `(Option T)`
  fields in rjungemann/turmeric-spices spices/json.
status: RESOLVED 2026-06-21 (gap G5) -- BOTH SITES FIXED on branch
  claude/g2-carrier-concrete-abi-audit-3yzkhm and VERIFIED end-to-end against the
  real turmeric-spices `json/encode` derive-json (the sibling was cloned into the
  environment and yyjson built; the emitted C compiles, links, and runs -- a
  `User { id : int  nick : (Option cstr) }` round-trips `{"id":7,"nick":"al"}`).
  Site 1 closed by the G9 fix (`field_read_emits_byvalue_aggregate`); Site 2
  closed by a forward-typedef fix (`emit_registered_struct_app_rec`, fixture
  tests/fixtures/result-over-struct-with-option-field-typedef-order). Found
  2026-06-21 as a follow-up to #482. See "Status update" below.
---

## Resolution (2026-06-21) -- BOTH SITES verified against real derive-json

Both sites are fixed and the whole `derive-json`-over-`(Option T)` path was
verified end-to-end against the real `json/encode` derive-json (the
turmeric-spices sibling was cloned into the environment, yyjson built): a
`User { id : int  nick : (Option cstr) }` derive-json round-trips
`{"id":7,"nick":"al"}` -- the emitted C compiles under `-std=c99 -Wall`, links
against `libturi.a` + yyjson, and runs correctly. Site 1 (the
`tur_option_t *` reconstruction on a field-read Option passed into `encode`) is
gone; Site 2 (the `Result__User` / `User` typedef ordering) resolves via the
forward typedef. Suite green (1744/0). The promotable round-trip fixture lives
in the turmeric-spices repo (it needs yyjson), not this compiler suite.

### Status update (2026-06-21) -- Site 1 resolved by the G9 fix

Site 1's defect is the stale `tur_option_t *` reconstruction inserted when an
`Option` value read from a struct field is passed to a typeclass method. The G9
fix (`field_read_emits_byvalue_aggregate` in `emit_expr.c`, see
`docs/archive/constrained-instance-dispatch-parametric-container-element-collapse.md`)
suppresses exactly that reconstruction: it resolves the field type through the
receiver's concrete type, recognizes the field read as already a by-value
aggregate, and skips the carrier->concrete bridge -- so `(.field x)` is passed
directly as the embedded `Option__T` instead of being cast through
`(tur_option_t *)(intptr_t)`.

A self-contained reproduction of Site 1's mechanism -- a `(Option int)` struct
field dispatched through a typeclass method whose instance is `Enc [Option]` --
now compiles and prints correctly. The original Site 1 repro uses
`json/encode`'s `Encode`/`Decode` (in the turmeric-spices sibling, not present
in this checkout), so this report stays open until that exact path is verified.

### Site 2 -- FIXED (forward typedef for struct-app-referenced user structs)

Reproduced self-contained: a `User` struct with an `(Option cstr)` field, used
in `(Result User cstr)`, emitted `Result__User__cstr { User *ok_val; }` *before*
the `User` typedef (which #482 pushes later once it depends on `Option__cstr`):
`error: unknown type name 'User'` + the `ok_val` int-fallback cascade.

Root cause: the embedding struct's field flush
(`type_codegen_emit_struct_apps`) drains ALL pending struct-apps into the early
typedef section -- including `Result__User__cstr`, which references `User *` --
ahead of the `User` typedef, and the struct-app dependency recursion only
follows TY_APP fields, never a user-struct (TY_STRUCT) pointer field.

Fix: `emit_registered_struct_app_rec` (`types.c`) now emits a guarded forward
`typedef struct User User;` when a struct-app field resolves to a user struct,
so the `User *` reference resolves; the later full `typedef struct User {...}
User;` is an accepted redundant typedef (verified under `-std=c99 -Wall`, the
generated-C flags). Pointer fields only need the name, so the forward decl
suffices. Fixture
`tests/fixtures/result-over-struct-with-option-field-typedef-order`. Suite green
(1742/0, no snapshot drift -- the forward decl only fires for struct-apps that
reference a user struct).

---

# Option's `tur_option_t` special-casing wasn't updated for #482

## One-line summary

#482 ("lay out by-value parametric struct defstruct fields as the embedded
aggregate") fixed the *layout* -- an `(Option cstr)` field is now an embedded
`Option__cstr` -- but two **Option-specific** codegen paths still assume the
*old* "struct fields hold Options as a `tur_option_t *` heap box"
representation. As a result an `(Option T)` struct field miscompiles while
every other by-value/heap parametric field type works.

## The control that localizes it

A `derive-json` struct compiles + round-trips for **every** by-value/heap
parametric field type EXCEPT `Option`:

| field type | `derive-json` (both dirs) |
|---|---|
| `Inner` (user by-value struct) | OK -- 0 errors |
| `(Pair int int)` (by-value builtin) | OK -- 0 errors |
| `(Cons int)` (`:heap` builtin) | OK -- 0 errors |
| `(Option int)` / `(Option cstr)` | FAIL |

`Pair` is the clean reference: same shape as `Option` (a by-value parametric
struct field, passed by value into the `encode` typeclass method), and it
works. So the general aggregate-field machinery from #482 is correct -- only
`Option`'s special-casing is stale.

## Site 1 -- typeclass-method arg lowering for an Option read from a struct field

Minimal repro (no derive-json):

```turmeric
(defmodule iso (export) (import json/encode)
(defstruct Box [o : (Option int)])
(defn main [] : int
  (let [b (make-struct Box (:: (some 7) (Option int)))]
    (println (encode (:: (some 7) (Option int))))   ;; A: Option LOCAL  -> OK
    (println (encode (:: (.o b)   (Option int))))   ;; B: Option FIELD  -> ERROR
  ) 0))
```

`error: aggregate value used where an integer was expected`. Emitted C for B:

```c
tur_option_t *__t = (tur_option_t *)(intptr_t)((x).nick);          // (!) aggregate -> ptr
... __inst_Encode_encode_Option__spec__...(
      (__t ? (Option__cstr){.is_some=__t->is_some,
                            .value=(const char*)(intptr_t)(__t->value)}
           : (Option__cstr){0})) ...                                // reconstruct aggregate
```

When an `Option` value that came from a **struct-field read** `(.field x)` is
passed to a typeclass method, the dispatch inserts a `tur_option_t *` heap-box
-> aggregate reconstruction. Post-#482 the field is *already* an `Option__cstr`
aggregate, so `(tur_option_t *)(intptr_t)((x).nick)` casts an aggregate to a
pointer. Notes:

- An `Option` **local** (`(some 7)`) into the same `encode` does **not** insert
  this -- it passes the aggregate directly (which is why standalone
  `Encode [Option]` works).
- Passing the same field-read `Option` into a **plain** `defn` (not a typeclass
  method) also works. The bug is the intersection: *field-read Option* +
  *typeclass dispatch*.
- `Pair` field-read into `encode` (via derive-json) works -- no `tur_pair_t *`
  round-trip is inserted. The reconstruction is hard-wired to `Option`.

**Fix:** read an `Option` struct field as the embedded aggregate (`(x).field`,
like `Pair`), not via a `tur_option_t *` reconstruction, when lowering it as a
typeclass-method argument.

## Site 2 -- `Result__T` type-emission ordering when `T` embeds an `Option`

For the full `derive-json` (which also emits the `Decode` instance returning
`(Result T cstr)`), a struct that embeds an `Option` field also miscompiles its
**type ordering**:

```c
typedef struct Option__cstr { ... } Option__cstr;          // 2468
typedef struct Result__User__cstr { ... User * ok_val; ... } // 2476  -> error: unknown type name 'User'
typedef struct User { int64_t id; Option__cstr nick; } User; // 2510  (emitted AFTER Result__User)
```

`error: unknown type name 'User'` (and a cascade: `Result__User__cstr.ok_val`
falls back to `int`, so `ok_val__spec__User_...` hits
`error: incompatible types ... returning 'int' but 'User' was expected`).

Because `User` now depends on `Option__cstr`, the emitter pushed `User` later --
but did **not** push `Result__User__cstr` (which embeds `User *`) after it. The
transitive order `Result__User -> User -> Option__cstr` isn't honored. The
identical struct with a `Pair`/`Cons`/user-struct field orders correctly
(`Result__T` after `T`), so again this only trips when the embedded field is an
`Option`.

**Fix:** order `Result__T` (and any type embedding `T`/`T*`) after `T` once `T`
gains a dependency on an `Option` aggregate field -- or forward-declare the `T`
typedef. (A plain `struct T;` forward decl is insufficient here because the
field is the `User` *typedef*, not `struct User`.)

## After the two fixes

The `Decode` body's field *reconstruction* is already correct --

```c
.nick = ok_val__spec__Option__cstr_...(__inst_Decode_decode_Option__spec__...(doc,
            json_obj_get(doc, val, "nick")))
```

-- it assigns the decoded `Option__cstr` straight into the embedded `.nick`, so
once Site 1 (encode field read) and Site 2 (type ordering) are fixed, the
`derive-json` round-trip over an `(Option T)` field should compile and
round-trip with no further changes. The container instances themselves
(`Encode`/`Decode [Option]`, `Encode [Cons]`, `decode-list`) already work
standalone.

## Scope

Compiler-side, `Option`'s residual `tur_option_t` special-casing (typeclass-arg
lowering + type-emission ordering) not reconciled with #482's embedded-aggregate
field layout. `Pair` is the working reference for both sites.

## Related

This is the **Option-specific residual special-casing** corner of the same
carrier <-> concrete ABI family tracked in
`docs/carrier-concrete-abi-crossing-audit-plan.md` (gap G5).

- **G3** (`instance-method-byvalue-struct-field-receiver-abi-mismatch.md`) is
  the closest sibling: it is *also* "by-value struct-**field** read of a
  parametric aggregate, passed into a typeclass method, post-#482." G3's defect
  is the *callee* taking the int64 carrier parameter; this bug's Site 1 is the
  *caller* inserting an Option-only `tur_option_t *` reconstruction. They meet
  at the same call boundary (field-read aggregate + typeclass dispatch) from
  opposite sides. The decisive difference is that G3 reproduces for any
  applied-struct head, whereas this reproduces **only for `Option`** (Pair is
  the working control) -- so this is a stale Option special-case, not the
  general dispatch-ABI gap.
- **G2** (`constrained-instance-dispatch-nested-parametric-element-carrier-collapse.md`)
  is the nested-container dispatch case; this bug is single-level and
  Option-keyed. Both are downstream of the same "concrete element vs carrier"
  tension but trip different machinery.
- Site 2 (type-emission ordering) is not a dispatch crossing at all -- it is a
  topological-sort gap in the typedef emitter exposed by #482 giving `T` a new
  dependency on an `Option` aggregate field. It rides along here because it is
  the second thing blocking the same `derive-json`-over-`Option` use case.
