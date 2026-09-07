---
title: Widening a union-typed value to `any` emits uncompilable C in every position
category: Reported
description: A union is already a `tur_tagged_t`, but the `any` widen re-tags it as though it were a scalar -- `TUR_TAG(38, (int64_t)(intptr_t)(x))` -- so cc rejects it with "aggregate value used where an integer was expected". Argument, return and local position all fail. The interpreter runs the same program and gives the right answers, so the semantics are settled and only the compiled back end is missing.
---

# A union-typed value cannot be widened to `any`

**Severity: medium.** A hard `cc` error, not a silent miscompile, so nothing
wrong ships -- but it blocks the one composition the gradual-typing story is
built on: keep an `any`-taking function and feed it a value whose type you have
since narrowed to a union.

Found while fixing
[any-type-guide-examples-do-not-compile](any-type-guide-examples-do-not-compile.md).
The union/intersection guide's Gradual Typing section is exactly this shape --
`(defn typed-print [x : (int | cstr)] : nil (debug-print x))` -- so the guide
was advertising it. That example has been rewritten to something that compiles,
with a pointer here.

## Repro (v0.44.2, `9d12bace`)

```turmeric
(defn debug-print [x : any] : nil (println (type-of x)))

(defn typed-print [x : (int | cstr)] : nil
  (debug-print x))

(defn main [] : int (typed-print 42) (typed-print "hi") 0)
```

```
$ tur run p.tur
/tmp/tur-build/p_tur.c: In function 'typed_hyprint':
/tmp/tur-build/p_tur.c:7280:9: error: aggregate value used where an integer was expected
 7280 |         debug_hyprint(TUR_TAG(38, (int64_t)(intptr_t)(x)));
tur: cc invocation failed (status 256)

$ tur --interpret p.tur
int
cstr
```

The interpreter's answers are the *right* ones -- the `any` reports the
payload's own type, not "union" -- so this is a codegen gap, not an undecided
question about what the widen should mean.

## Every position, not just arguments

| widen position | program | result |
| --- | --- | --- |
| call argument | `(debug-print x)` where `x : (int \| cstr)` | `aggregate value used where an integer was expected` |
| return | `(defn to-any [x : (int \| cstr)] : any x)` | same |
| local | `(let [u (:: 42 (int \| cstr))] (debug-print u))` | same |

## Root cause

`elab_coerce_to_any` wraps the value in an `EX_UNION_INJECT` tagged with
`any_box_tag_for_type` -- the payload's `TypeKind`, here `TY_UNION` (38). The
emitter's `EX_UNION_INJECT` case then falls into its final arm:

```c
buf_printf(&out, "TUR_TAG(%lld, (int64_t)(intptr_t)(%s))", tag, inner);
```

which is the *scalar* arm. A union value is not a scalar: it is already a
`tur_tagged_t { int64_t tag; int64_t val; }`, so the cast has no meaning and cc
rejects it. The two arms above it (the float bit-pattern arm and the by-value
ADT heap-box arm) both exist precisely because their payloads do not ride the
carrier as an integer -- a union is a third such payload and has no arm.

## The real question the fix has to answer

Not "how do I cast this aggregate", but **what tag should the `any` carry**.

An `any` box's tag is a `TypeKind` or a per-monomorph type id; a union's tag is
a **member index**. They are different namespaces, so the two representations
are not interchangeable despite having the same C layout -- re-tagging in place
is wrong even though it would compile.

## Fix directions

1. **Re-box through the member.** At the widen, switch on the union's member
   index and inject the payload with *that member's* `any` id, so the `any`
   carries `int` / `cstr`, not "union". This matches what the interpreter
   already answers and what a reader expects, and it composes with `is?` /
   `cast` on the result with no further work. Costs a small switch at each widen
   site -- the member set is statically known, so it is a jump table, not a
   search.
2. **Reject it with a diagnostic** as an interim, the way
   `any-narrowing-broken-for-parametric-receivers` and
   `forall-dict-byvalue-receiver-emits-uncompilable-c` did. Cheap, and it turns
   a `cc` error a reader cannot act on into one they can. Strictly worse than
   (1) -- it removes a capability the interpreter already has -- so pair it with
   a note rather than landing it alone.

Whichever lands, assert it **on both back ends**: the interpreter already gets
this right, so a fixture that only ran compiled would not notice the two
diverging again.

The fixture `tests/fixtures/docs-any-guide-examples` carries a comment marking
the line to add back when this is fixed.
