---
title: Map/Set Typed-Pointer Producer Slice -- Plan (for review)
category: Planning -- ABI / Codegen, end-to-end monomorphization
description: Replicate the Vec producer-monomorphization slice (commit "monomorphize Vec inline-C producers to typed pointers") for Map/Set/MutableMap. RECORDS A KEY FINDING -- the audit shows Map/Set currently have ZERO producer-result crossings, so this slice is consistency/future-proofing, not a crossing-reduction win. Written for review; the recommendation is to DEFER Map/Set until M4 dict-ABI creates typed Map/Set consumers, and to do the small MutableMap producer win opportunistically.
---

# Map/Set Typed-Pointer Producer Slice -- Plan

## Status (as of 2026-06-28)

This plan is **COMPLETE.** Re-verified against the tree:

- [x] **Step 1 -- `type_is_heap_collection` allow-list.** Done. Verified in
      `src/compiler/emit_module.c:2011-2014`: the predicate (still named
      `type_is_heap_vec`) admits `Vec`, `Map`, `Set`, `MutableMap`. `Cons`
      is intentionally excluded per the Out-of-scope section.
- [x] **Step 2 -- carrier-force K/V element slots.** Done; covered by
      `tce3-map-cstr-val` and friends.
- [x] **Step 3 -- `__TUR_RET__` on heap-returning producers.**
  - [x] **Vec** -- #400 (prior baseline).
  - [x] **MutableMap** -- commit `4e7a8f77` (#411).
  - [x] **Map** -- #555 (representation flip from transparent int newtype to
        `(hamt :ptr<void>)`, then producer-typing).
  - [x] **Set** -- landed alongside Eq[Set] by-value receiver in the same
        window as the typed-collection-eq-consumers plan.
- [x] **Step 4 -- signature/bridge plumbing.** Already generic; no changes
      needed.

All four `:heap` collections (Vec, MutableMap, Map, Set) now type their
producers end-to-end. The legacy "COMPLETE -- 2026-06-25" banner and the
"for review" body below are preserved as the paper trail.

## COMPLETE -- 2026-06-25

**This plan is fully landed; all four `:heap` collections now type their
producers.** The "for review" body below (status verified 2026-06-22)
recommended *deferring* Map/Set producer-typing until M4 dict-ABI created
consumer demand. Both halves of that gate have since resolved, so the
recommendation is moot and the work is done:

- **M4 (per-method typeclass dict ABI) is landed**, not a future gate. Non-HKT
  instance dicts hold per-instance concretely-typed function pointers and
  dispatch with no `(int64_t)(intptr_t)` result cast; parametric instances
  route through M4c Path A per-instantiation specs. See
  `docs/archive/m4-typeclass-per-method-abi-plan.md` and
  `docs/artifacts/monomorphization-audit.md` ("M4-rest -- LANDED").
- **M6/M7 (HKT class dispatch) is landed and on by default**
  (`g_m7_hkt_enabled = true`, Option 1 full per-`(f, A)` monomorphization,
  2026-06-19). See `docs/archive/hkt-dispatch-options-tradeoff.md`.
- **Producer typing is done for every collection:** Vec (#400), MutableMap
  (#411), Map (#555), Set (this branch). Each mints typed producer specs
  (`Vec__A *` / `Map__K__V *` / `Set__A *` / `MutableMap__K__V *`) via
  `__TUR_RET__`, with the collection name in `type_is_heap_vec`'s allow-list.

The Map/Set slices were *not* held until a separate M4 regen window after all
-- once `Eq[Map]`/`Eq[Set]` were rewritten to by-value receivers (the
typed-collection-eq-consumers plan), the typed consumers appeared and the
producer typing landed in the same window, exactly as step 3 of the recipe
below describes. The audit table's "0 crossings for Map/Set" snapshot reflected
the *pre-consumer* tree; with by-value `Eq` consumers present the producer
typing removes real crossings. The body below is retained as the paper trail.

## Status (verified 2026-06-22)

- **TL;DR / Recommendation (defer Map/Set, do MutableMap now):**
  Honored. `src/compiler/emit_module.c:1913-1924` `type_is_heap_vec`
  allow-list is `{"Vec", "MutableMap"}` only -- Map/Set deliberately
  excluded.
- **Step 1 (generalize predicate to `type_is_heap_collection`):**
  Partial / scoped. Predicate kept name but extended to MutableMap
  (`emit_module.c:1922-1923`); Map/Set not added. Done as recommended.
- **Step 2 (carrier-force K/V element slots):** N/A for Map/Set
  (predicate doesn't fire). For MutableMap, validated by in-tree
  fixtures + `tce3-map-cstr-val` reference.
- **Step 3 (`__TUR_RET__` on heap-returning producers):**
  - **Map (`stdlib/map.tur`):** DONE (eq-map-typed-consumer report). Map is
    now a non-transparent `:heap` struct (`(hamt :ptr<void>)`); the seven
    heap-returning producers return through `__TUR_RET__`, `Map` joined the
    `type_is_heap_vec` allow-list, and the float/cstr carrier-forcing block was
    extended to recover the degenerate multi-param declared `(Map K V)` from the
    resolved slot type. `Eq[Map]` dispatches via a typed by-value spec; typed
    `(Map int int)` consumers receive `Map__int__int *`. See
    `docs/archive/eq-map-typed-consumer-blocked-on-transparent-newtype.md`.
  - **Set (`stdlib/set.tur`):** DONE. `Set` is already non-transparent
    (`(hamt :ptr<void>)`), so unlike Map it needed no representation change --
    only the producer/consumer flip, done in lockstep per the same rule the
    Map slice followed. The six heap-returning producers (`set-new`,
    `set-add`, `set-remove`, `set-union`, `set-intersect`, `set-diff`, plus the
    `set-add1` wrapper) now return through `(__TUR_RET__)(intptr_t)` and are
    `[A]`-polymorphic over `(Set A)`; the accessors (`set-count`, `set-member?`,
    `set-free`, `set-hamt`) and the `Eq[Set]` helpers (`set-eq-full` /
    `set-eq-driver`) take by-value `(Set A)`; `Set` joined the
    `type_is_heap_vec` allow-list (`emit_module.c`). The element/hash slots
    stay `:int` carrier (mirroring Map's `key :int`), and the carrier-essential
    `set-eq?` / `set-eq-cmp?` inline-C walkers stay on `:int` (the same
    compromise `map-eq?` / `vec-eq?` made). A concrete `(Set int)` now
    monomorphizes to `Set__int *` end to end: typed producers
    (`set_new__spec__Set__int__`, `set_add__spec__...`), typed accessors
    (`set_count__spec__...`), and `Eq[Set]` dispatching via
    `__inst_Eq_eq_qu_Set__spec__...(Set__int *, Set__int *)`
    (`set-typed-consumer` fixture). An HKT-headed / abstract-A receiver still
    instantiates the int64 carrier, so existing carrier callers are unaffected.
  - **MutableMap (`stdlib/mutmap.tur`):** DONE. `mutmap-new` at line
    73 returns through `__TUR_RET__`.
- **Step 4 (signature/bridge plumbing already generic):** Confirmed --
  unchanged.
- **MutableMap section ("the one worth doing now"):** Shipped as commit
  `4e7a8f77` "Fix MutableMap typed-pointer producer monomorphization
  (#411)" (2026-06-17), with prior plumbing in `e73c872f` (#397) and
  `9bb66e5a` (#391, Vec slice).
- **Validation harness / Risks / Out-of-scope / North star:** unchanged
  -- gated on M4 dict-ABI; no Map/Set regen window opened.

**Net:** no new work required against this plan until M4 makes typed
Map/Set consumers exist.

---

This is **step 3** of the M3 -> M7 sequencing in
[docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md](../../reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md):
replicate the **Vec producer slice** (commit `600e859`, "monomorphize Vec
inline-C producers to typed pointers (bucket A, Vec slice)") for the other
`:heap` collections -- `Map`, `Set`, and `MutableMap`.

## TL;DR -- read this before doing the work

**The audit says there is nothing to eliminate for Map/Set right now.** A
full-suite `TUR_M3_AUDIT=1` sweep on the post-Vec-slice tree
(`bash tests/run.sh`: 1649/0) bucketed the 93 residual carrier crossings by
type:

| Type | crossings | nature |
|---|---|---|
| `Vec int` | 80 | root 2 -- uniform-int64 `Eq[Vec]` dict slot + comparator thunks (M4 dict-ABI) |
| `MutableMap int int` | 4 | 2 root-1 (producer-result, `mutmap-new`) + 2 root-2 (carrier base) |
| `Result Device` / `Option Device` / `Result int cstr` | 7 | by-value-struct dispatch / blessed inline-C |
| `SChan<...>` | 2 | type-erased channel |
| **`Map`** | **0** | -- |
| **`Set`** | **0** | -- |

Direct grep confirms it: **zero `(Map__* *)(intptr_t)` / `(Set__* *)(intptr_t)`
casts** appear in any fixture's emitted C (`tce3-map-cstr-val`,
`map-of-tvec-eq`, `set-of-tvec-eq`, `map-basic`, `set-basic` all = 0). Map/Set
*values* are never typed anywhere -- their `Eq` path and all consumers stay on
the int64 carrier end-to-end, unlike Vec, whose `*-byval` twins created typed
consumers that the carrier producer had to feed via a cast.

So replicating the Vec slice for Map/Set would:

- edit ~10 inline-C producer bodies (add `__TUR_RET__`),
- force a coordinated snapshot regen across the ~1640 `expected.c`,
- and remove **zero** crossings.

The mutation-safety the matrix worried about (`mutmap-set!` reallocating a
by-value header copy) is **already delivered by the `:heap` flip** (Map/Set/
MutableMap are shared typed pointers at the boundary; the producer's *return*
ABI is orthogonal to that). So the producer-typing adds no safety either.

### Recommendation

1. **Defer Map/Set producer-typing** until there is consumer demand -- i.e.
   until **M4 dict-ABI** (root 2) types the `Eq[Map]` / `Eq[Set]` dispatch path
   and creates typed Map/Set consumers the way the Vec `*-byval` twins did for
   Vec. At that point the producer cast appears and this slice removes it; doing
   it before then is regen churn for no benefit.
2. **Do the MutableMap producer win opportunistically** -- 2 of its 4 crossings
   are root-1 (`a = mutmap-new(); (.eq? a b)` casts the int64 `a` to
   `MutableMap__int__int *`). Typing `mutmap-new` + the MutableMap accessors
   clears those 2. Small and self-contained; see "MutableMap" below.
3. When Map/Set *are* done, do them **with** the M4 dict-ABI change in one
   coordinated regen window, not as a standalone churn.

The rest of this doc is the concrete recipe, so the work is turnkey whenever the
demand materializes.

## Mechanism (identical to the Vec slice)

The Vec slice landed four coordinated pieces. Map/Set/MutableMap reuse all four;
only the per-type predicate and the producer bodies differ.

### 1. Generalize the collection predicate

`type_is_heap_vec` (`emit_module.c`) is the Vec-only gate. Generalize it to a
`type_is_heap_collection` (or add Map/Set/MutableMap to its allow-list) keyed on
the struct constructor name:

```c
static bool type_is_heap_collection(Type t) {
    if (!type_is_heap_struct(t)) return false;
    StructDef *def = /* extract via type_extract_struct_app / t.as.struct_.def */;
    if (!def || !def->name) return false;
    return strcmp(def->name, "Vec") == 0
        || strcmp(def->name, "Map") == 0
        || strcmp(def->name, "Set") == 0
        || strcmp(def->name, "MutableMap") == 0;
}
```

Then replace the three `type_is_heap_vec` call sites in `emit_abi_register_call`
(the arg gate, the result gate, and the slot-forcing "keep this slot typed"
test) with `type_is_heap_collection`. The concrete->carrier bridge in
`emit_expr.c` already uses the broad `type_is_heap_struct`, so it needs no
change.

**The declared-type keying is load-bearing.** The gate keys on
`fd->param_types[i]` / `fd->return_type` carrying a *structural* `(Map K V)` /
`(Set A)` -- NOT the resolved arg. This is what keeps the generic `some`/`ok`
constructors (whose element `A` merely *resolved* to a Map/Set) off the slice,
exactly as it did for Vec. Do not regress this to a resolved-type check.

### 2. Carrier-force the element slots (float/cstr safety)

The Vec slice's most important correctness piece. In a collection-producer spec,
ONLY the `(Map K V)` / `(Set A)` slots are typed to their pointer; every
element/scalar slot (`K`, `V`, the raw `:int` carrier keys, `keyeq`, `owned`,
...) is forced to `TYPE_INT`. The inline-C bodies read those with a *bit
reinterpret* (`(void*)(intptr_t)val`, `return (int64_t)(intptr_t)...`), which a
monomorphized `double` / `const char *` slot would turn into a numeric
conversion.

This is not hypothetical: the first (un-scoped) cut of the Vec slice broke
`tce3-map-cstr-val` (`0.5 -> 0`) and emitted `const char *`-from-`int64`
warnings *precisely because* Map's `V=cstr`/`V=double` slots were being
monomorphized. The carrier-forcing loop already handles this -- it forces every
non-`type_is_heap_collection` slot to `TYPE_INT` -- so once Map/Set join the
predicate, their `K`/`V` slots are correctly forced. **Verify** by re-running
`tce3-map-cstr-val` (expected `0.5`) and a `Map cstr`-valued fixture after the
change.

Note the collision hazard the forcing avoids: with `K`/`V` forced to int64, two
Map specs that differ only in value type (`Map int cstr` vs `Map int double`)
keep DISTINCT clone names because the `(Map K V)` slot retains its element type
in the header name (`Map__int__cstr *` vs `Map__int__double *`). If the Map slot
itself were ever forced (the bug the un-scoped cut hit), the names collapse and
clang reports a redefinition. Keeping the Map/Set slot typed is what prevents
this.

### 3. `__TUR_RET__` on the heap-RETURNING producers

`__TUR_RET__` (`emit_core.c`) expands to the active spec's concrete result C
name (`Map__int__cstr *`) or `int64_t` for the carrier base. Every inline-C
producer whose declared return is `(Map K V)` / `(Set A)` must change its body's
return cast from `(int64_t)` to `(__TUR_RET__)`:

```c
return (__TUR_RET__)(intptr_t)r;   // was: return (int64_t)(intptr_t)r;
```

**Map heap-returning inline-C producers** (`stdlib/map.tur`):
`map-new` (:45), `map-wrap` (:75), `map-assoc-eq-o` (:96), `map-dissoc-eq-o`
(:131), `map-assoc-eq` (:170), `map-dissoc-eq` (:247), `map-merge` (:564).
(`map-empty-for` is pure-Turmeric `(map-new)`, not inline-C -- it rides the
normal monomorphization path, no `__TUR_RET__` needed. `map-get-eq-o` returns
`:V`, an element, NOT heap -- its result is carrier-forced to int64, no
`__TUR_RET__`.)

**Set heap-returning inline-C producers** (`stdlib/set.tur`): audit the file for
`: (Set A)` inline-C bodies (`set-new`, `set-add`, `set-remove`, ...) and apply
the same `(int64_t)` -> `(__TUR_RET__)` change to each.

The accessors that return an *element* (`map-get-eq-o : V`, `set-...`) or a
scalar (`map-count : int`, `set-count : int`, `map-has? : bool`) do NOT use
`__TUR_RET__` -- their result is carrier-forced to int64 and the call-site
ascription reinterprets, identical to `vec-get`.

### 4. The signature/bridge plumbing is already generic

- `emit_fns.c` `typed_heap_spec` already types any inline-C `:heap` result under
  an active spec (`type_is_heap_struct(rft)`) -- works for Map/Set unchanged.
- The spec forward decl / header decl already use `type_c_name(spec->result_type
  / arg_types[i])` -- typed pointers for Map/Set automatically.
- The `emit_expr.c` concrete->carrier bridge uses `type_is_heap_struct` -- fires
  for a typed `Map__* *` reaching an int64 carrier consumer with no change.

So pieces 1-3 are the entire diff: one predicate generalization, the
carrier-forcing already covers K/V, and the per-body `__TUR_RET__` edits.

## MutableMap (the one worth doing now) -- DONE

**Landed (TCO-in-ABI-specs MutableMap follow-up).** `mutmap-new` returns through
`__TUR_RET__` and `type_is_heap_vec` accepts `MutableMap`, so it mints
`mutmap_new__spec__MutableMap__int__int__()`. The "Multi-param caveat" below was
a misdiagnosis -- the zero-arg `[K V]` producer's binding/result resolution was
already correct; what blocked it was the missing `__TUR_RET__` (the producer-
result gate keys on `fd->return_type`, which is a degenerate `type_from_kind`
shell for a multi-param TY_APP, so the `__TUR_RET__` / `abi_changes` intern path
is the one that fires). A SEPARATE general call-site relabel bug (a typed `:heap`
value spilled to int64 when passed to a user fn taking the concrete heap type --
Vec hit it too) was fixed in emit_expr.c via `callee_param_is_typed_heap_ptr`.
See `docs/archive/mutmap-multi-param-producer-typing-blocked.md`. The rest of
this section is retained as the historical analysis.

MutableMap is the only collection with a live producer-result crossing. In
`mutmap-eq`, `a = mutmap-new(); b = ...; (.eq? a b)` emits

```c
__inst_Eq_eq_qu_MutableMap__spec__...((MutableMap__int__int *)(intptr_t)(a_885), ...)
```

-- `a_885` is an `int64_t` local because `mutmap-new` returns the carrier; the
typed `Eq[MutableMap]` spec then casts it. Typing `mutmap-new` (and the
`:heap`-param accessors `mutmap-set!`/`-get`/`-has?`/`-delete!`/`-len`/`-free`)
makes `a_885` a `MutableMap__int__int *` and drops the 2 root-1 crossings.

**Multi-param caveat (the documented MutableMap blocker).** MutableMap is
`(MutableMap K V)` -- two type params. `#364`'s message and the M5 docs note the
multi-param instance path records the type-ctor param as `TY_STRUCT` not
`TY_TYVAR` in some positions. Confirm the `Eq[MutableMap]` spec's `arg_types`
resolve to a concrete `MutableMap__int__int` (they do in `mutmap-eq` today --
the byval spec already exists) before relying on the producer spec matching. If
the multi-param resolution is incomplete, the producer spec may not match the
consumer's arg type and the cast stays; that is the `#364` follow-up, separate
from this producer-typing.

The remaining 2 MutableMap crossings (line ~3342, inside the
`__inst_Eq_eq_qu_MutableMap(int64_t, int64_t)` carrier base) are root-2 -- they
clear with M4 dict-ABI, not here.

## Validation harness (per the project STRICT rules)

1. `bash tests/run.sh`: zero new `FAIL`. **Snapshot regen will be large** -- a
   Map/Set ABI change regenerates a big fraction of the ~1640 `expected.c`. Do
   it as ONE coordinated commit (Fixture STRICT RULE), and coordinate timing
   with in-flight Map/Set-touching branches. (The Vec slice happened to need
   zero regen because the new specs only appeared in snapshot-less fixtures;
   Map/Set will NOT be so lucky once their consumers are typed.)
2. `bash tests/run-turi.sh`: interpreter parity. The Map/Set producers stay
   inline-C, so `native_map_*` / `native_set_*` overrides keep firing. Expect
   the baseline `1206 passed, 2 failed` (the documented
   `eq-carrier-capturing-comparator` / `mutmap-eq`).
3. Re-run `tce3-map-cstr-val` and a `Map`-with-`cstr`/`float`-value fixture
   explicitly -- the float/cstr-slot carrier-forcing is the highest-risk piece.
4. Spice roundtrip `../turmeric-spices/spices/{ecs,json,frame}` -- `frame` is the
   heavy Map user; expect only the documented pre-existing failures.
5. `TUR_M3_AUDIT=1` sweep: confirm the Map/Set crossing count (target: whatever
   M4 dict-ABI exposed; 0 today, so no movement until M4 lands first).

## Risks

- **Snapshot blast radius** (the dominant cost): a Map/Set return-ABI change
  touches every program that links the map/set stdlib once their consumers are
  typed. One regen window.
- **Float/cstr value slots** (the dominant correctness risk): the carrier-forcing
  must keep `V`/`K` on the int64 carrier. Pinned by `tce3-map-cstr-val`.
- **MutableMap multi-param resolution**: the `#364` `TY_STRUCT`-vs-`TY_TYVAR`
  gap may block the producer spec from matching the consumer; verify before
  relying on it.
- **Zero benefit today** (the dominant strategic risk): doing this before M4
  dict-ABI is pure churn. Defer.

## Out of scope

- M4 dict-ABI (root 2) -- the 80 Vec + 2 MutableMap carrier-base crossings.
  That is the dominant lever and is being pursued first.
- `Cons` typed-pointer producer-typing -- `Cons` is `:heap` but its `Eq[Cons]`
  is deliberately carrier-based (#369); revisit only if a typed Cons consumer
  appears.

## North star

Once M4 dict-ABI types the `Eq[Map]`/`Eq[Set]` dispatch, a `#map{...}` literal
or `(map-assoc m k v)` produces a `Map__K__V *` that flows into the typed
instance method with no carrier round-trip -- the same end state the Vec slice
reached, achieved by the same four-piece recipe above.
