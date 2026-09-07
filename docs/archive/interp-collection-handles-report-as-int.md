---
title: Under `--interpret`, a Vec/Map handle in an `any` reports `type-of` "int", and `is?` is wrong in both directions
category: Archive
description: The interpreter represents a vector or map as a raw TURI_INT holding a pointer, so `(defn f [] : any (vec-of 1 2 3))` gives type-of "Vec" compiled and "int" interpreted. Worse than a name: `(is? v (Vec int))` is a false NEGATIVE and `(is? v int)` a false POSITIVE, so a type-case dispatching on int takes the wrong arm for a vector.
---

# A collection handle in an `any` is an `int` under `--interpret`

**RESOLVED 2026-09-07** via fix direction 2 -- tag at the widen -- which turned
out to mean the interpreter's widen has to *box*, since `EX_UNION_INJECT` was
the identity there and there was nothing to tag.

The interpreter now wraps a widened payload when the runtime value cannot answer
for its own type, and it boxes on **exactly the condition the compiled widen
mints an id on** (a named type: an ADT with a def, or an applied constructor
whose head resolves to one). That is what makes the two agree rather than
merely both being non-silly. Three payloads are deliberately not boxed: a
TuriStruct already carries its own name, a primitive's runtime tag was always
the right answer, and a closure has its own reflection path
(`any-fn-tag-does-not-discriminate-signatures`) that boxing would have broken.

Both directions of the defect are closed, and the handle stays usable:

| | before (compiled / interp) | after (both) |
| --- | --- | --- |
| `type-of` on a `(Vec int)` | `Vec` / `int` | **`Vec`** |
| `is? v (Vec int)` | 1 / 0 | **1** |
| `is? v int` | 0 / 1 | **0** |
| `vec-len (cast v (Vec int))` | 3 / 3 | **3** |

## Two things worth recording

**The box never escapes, and that is checkable rather than hopeful.** An `any` is
opaque to everything except `type-of` / `is?` / `cast`, and the only route to a
payload is `cast` -- *including* an `is?` guard, because narrowing elaborates to
`(let [x (cast x T)] ...)` (`if_narrow_branch` in `elab_forms.c`). So one unwrap,
in `EX_ANY_CAST`, covers every consumer. The fixture exercises the narrowing path
specifically, not just the explicit cast.

**The first attempt was dead code, and instrumenting found it in one step.** The
obvious place -- `EX_UNION_INJECT` in `eval_expr_impl`'s switch -- is not the
live site: the explicit-stack driver descends the node via `unary_operand` and
finishes in `eval_unary_post`, whose `default:` arm ("transparent shims") is what
actually made the widen an identity. A probe printing at the switch produced *no
output at all*, which said "wrong site" immediately where reading control flow
had said "right site". Both arms now call one helper so they cannot drift.

## Two neighbouring defects the probes turned up

Neither is this bug; both are filed separately.

- [interp-inline-c-opaque-segv-in-any-reflection](../reported/interp-inline-c-opaque-segv-in-any-reflection.md)
  -- **a segfault.** A `defopaque` value returned from an inline-C body arrives
  TURI_STRUCT-tagged with `as_struct` set to the raw integer the body returned,
  and `turi_any_named_type` dereferences it. Pre-existing, and invisible because
  `run-turi.sh` skips every inline-C fixture.
- [any-drop-inlining-warns-free-nonheap](../reported/any-drop-inlining-warns-free-nonheap.md)
  -- gcc warns `'free' called on a pointer to an unallocated object '7'` when it
  inlines `__tur_any_drop` at a site whose payload is a constant. The registry
  row correctly says `boxed = 0` and nothing is freed; it is warning noise in a
  user's build, not a defect in the program.

Pinned by `tests/fixtures/any-collection-handle-reflects`, which both suites run.

---

**Severity: medium-high.** Wrong in both directions, silently. A `type-case`
over an `any` that has an `int` arm and a `Vec` arm takes the `int` arm for a
vector under the interpreter and the `Vec` arm compiled -- the same program,
two answers, no diagnostic.

Found while fixing
[interp-native-ctor-loses-adt-name](../archive/interp-native-ctor-loses-adt-name.md),
by running the sweep that report asked for ("a fixture that round-trips
`type-of` for each natively-constructed stdlib value ... and compares the two
back ends"). It is a **different root cause** from that one -- there the value
was a struct missing its constructor link; here there is no struct at all -- so
it is filed separately rather than absorbed.

## Repro (2026-09-07, after the native-ctor fix)

```turmeric
(defn v [] : any (vec-of 1 2 3))
(defn m [] : any (hamt-of 1 2))

(defn main [] : int
  (println (type-of (v)))
  (println (type-of (m)))
  (println (if (is? (v) (Vec int)) 1 0))
  (println (if (is? (v) int) 1 0))
  0)
```

```
$ tur run p.tur              $ tur --interpret p.tur
Vec                          int
Map                          int
1                            0        <- false negative
0                            1        <- false positive
```

## Root cause

The interpreter's collection natives return a raw integer carrier, not a
TuriStruct:

```c
/* src/turi/collections_native.c:1051 -- native_vec_new */
int64_t *v = (int64_t *)calloc(4, sizeof(int64_t));
...
TuriValue r = {0}; r.tag = TURI_INT; r.as_int = (int64_t)(intptr_t)v; return r;
```

`EX_ANY_TYPE_OF` maps `TURI_INT` to `"int"`, and `EX_ANY_IS` maps it to
`TY_INT`, so both questions are answered about the carrier rather than about
what it carries. The compiled side has a real type for the handle and interns a
per-type box id for it, which is why the two disagree.

This is a **representation** gap, not a missing link: there is no per-value
place to hang the type, because the value is an integer.

## Scope

Every collection whose interpreter representation is a bare handle. Confirmed
for `vec-of` and `hamt-of`; `cons` / `nil-value` show the same `"int"` shape in
a quick probe. Anything built by `defstruct`/`defdata` is unaffected (it is a
TuriStruct and carries its name). The sweep to run before fixing is the same
one that found this: `type-of` and `is?` over each collection builder, compared
across back ends.

## Fix directions

1. **Give the interpreter a typed handle.** A small wrapper value -- a TuriStruct
   whose single field is the carrier, or a new `TuriTag` carrying `{kind,
   ptr}` -- would let `type-of` and `is?` answer about the collection. This is
   the honest fix and the one that generalises to every opaque handle, but it
   touches every collection native and every consumer that reads `a[0].as_int`,
   so it is not small.
2. **Tag only at the widen.** `EX_UNION_INJECT` knows the value's STATIC type,
   which is `(Vec int)` -- so the `any` box could record it there even though the
   runtime value cannot. That is exactly what the compiled path does, and it is
   local to the widen/`is?`/`cast` trio rather than to the collection natives.
   Cheaper, and it fixes the whole observable surface; it does not help anything
   that asks about a bare handle outside an `any`, which today is nothing.

Direction 2 looks like the better trade and is worth measuring first. Either
way, assert **both back ends** in the fixture -- this survived because nothing
compared them on a collection payload.
