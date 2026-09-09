---
title: Under `--interpret`, every element of a heterogeneous Vec reports the type of the LAST one pushed
category: Reported
description: The interpreter's Vec buffer is raw int64 cells with a single element tag per vector -- native_vec_push calls it "the homogeneous element tag" -- so vec-get re-tags every element with it. A (Vec any) holding an int, a cstr, a float and a bool reports bool for all four, and a cast to the element's real type panics. The compiled back end answers correctly.
---

# The interpreter's Vec keeps one element tag for the whole vector

**Severity: medium-high -- it is a WRONG ANSWER, not a gap.** No diagnostic, no
crash at the read; `type-of` simply lies, and a `cast` to the type it lied about
panics with a message naming a type the program never asked for.

Found while landing saffron-lang-plan S6, which made a `(Vec any)` element
round-trip with its own tag on the compiled path. The interpreter was never
exercised on a heterogeneous container before, because until S6 the element
could not be read back as an `any` at all.

## Repro

```turmeric
(defn dyn [x : any] : any x)
(defn main [] : int
  (let [v (:: (vec-new) (Vec any))]
    (vec-push! v (dyn 1))
    (vec-push! v (dyn "hi"))
    (vec-push! v (dyn 7.1))
    (vec-push! v (dyn true))
    (println (type-of (vec-get v 0)))
    (println (type-of (vec-get v 1)))
    (println (type-of (vec-get v 2)))
    (println (type-of (vec-get v 3)))
    (println (cast (vec-get v 2) float)))
  0)
```

| back end | output |
|---|---|
| compiled | `int` `cstr` `float` `bool` `7.1` |
| `--interpret` | `bool` `bool` `bool` `bool`, then `panic: cast: any holds bool, not float` |

`tests/fixtures/vec-any-element-roundtrip` pins the compiled answer and carries
`requires.compiled` so this divergence is recorded rather than hidden by a
skip nobody reads.

## Root cause

`native_vec_push` (`src/turi/collections_native.c`) stores the payload word into
a raw `int64_t` buffer and records ONE tag for the whole vector:

```c
data[len] = a[1].as_int;
v[1] = len + 1;
/* Record the homogeneous element tag so vec-get/pop re-tag float/cstr/bool
 * carriers (the buffer only holds raw int64 cells). */
if (a[1].tag != TURI_INT) vec_tag_set(v, (uint8_t)a[1].tag);
```

The comment is accurate about its own assumption: the design is for a
homogeneous container, where one tag per vector is all the information there is.
`vec-get` re-tags with it, so the last non-int push wins for every index.

That assumption held for as long as the type system enforced homogeneity.
`(Vec any)` is the first element type for which it does not, and `any` is
exactly the type whose whole content is the per-value tag.

## Resolution (2026-09-07)

**Fixed via direction 1**, a byte per element rather than one tag per vec. The
side table stays keyed on the vec header pointer and stays process-lifetime;
only the payload widens, from `uint8_t tag` to `uint8_t *tags` grown with the
vec. An index past what has been recorded reads `TURI_INT` -- the same default
an absent key already gave -- so a vec built by another native (the schema/json
int64 buffers the original comment names) is untouched.

Five call sites took an index: the two writers (`vec-push!` at the slot it just
wrote, `vec-set!` at its index) and the three readers (`vec-get`, `vec-pop!`,
and the GC mark walk, which types each cell to mark it).

`tests/fixtures/vec-any-element-roundtrip` now runs on BOTH back ends and they
agree: `int` / `cstr` / `float` / `bool`, with `(cast (vec-get v 2) float)`
giving 7.1. It had carried `requires.compiled` for exactly one commit, to record
the divergence rather than hide it.

Suites: `run.sh` 2865/0, `run-turi.sh` 1957/0, `run-leak-check.sh` 91/0 with one
known-open. The change touches every interpreted Vec, which is why both suites
ran before it landed.

**Map and Set were flagged in this report and are NOT answered by it.** They
carry no equivalent of `vec_tag_set` (grep finds none), so they do not share
this mechanism -- but whether their value reads preserve a per-entry tag is a
separate question, and the surface (`map-assoc!` does not exist; the Map is the
persistent HAMT) is different enough that one probe did not settle it. It should
be settled before S6's `#map{...}` / `#set{...}` work, not after.

## Fix directions

1. **A per-element tag array beside the buffer.** The Vec header already has
   room for the side table (`vec_tag_set` writes one today); widening it to one
   byte per element mirrors what the compiled path does with a boxed element,
   at one byte rather than one allocation. Cheapest change that makes the two
   back ends agree.
2. **Store a boxed TuriValue per element** for a `(Vec any)` only, keyed on the
   element type, so the interpreter matches the compiled representation exactly.
   More faithful, and it prices every vector by element type rather than
   uniformly.
3. **Reject a heterogeneous push** at run time. Not viable -- `(Vec any)` exists
   precisely to hold one.

Direction 1 first: it is local to `collections_native.c`, and the compiled path
already fixes the semantics the interpreter should match.

The same question applies to the interpreter's Map and Set element storage, and
should be checked there before S6's `#map{...}` / `#set{...}` work rather than
after.
