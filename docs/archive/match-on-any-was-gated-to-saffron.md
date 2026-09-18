# `match` on an `any` scrutinee was gated to Saffron, so plain Turmeric emitted uncompilable C

**Severity: medium (a hard `cc` error with no Turmeric-level diagnostic).**
**RESOLVED 2026-09-18**, found while writing a plain-Turmeric probe for
[any-widen-stored-in-an-adt-field-has-no-owner](../reported/any-widen-stored-in-an-adt-field-has-no-owner.md).

## Repro

```turmeric
(defdata Lst [] (Cons [hd : any tl : any]) (Nil))
(defn peek [l : any] : int (match l (Cons h t) 1 (Nil) 0))
(defn main [] : int (println (peek (:: (Nil) any))) 0)
```

```
error: invalid initializer
  tur_adt_Lst __scrut_v = (l);
tur: cc invocation failed (status 256)
```

Adding `#lang saffron` as the first line -- changing nothing else -- compiled
and ran the same program.

## Root cause

`src/compiler/elab_structs.c`, the `any`-scrutinee narrow:

```c
if (scrutinee->type.kind == TY_ANY && lang_span_is_saffron(call->span)) {
```

The narrow reads the arms, finds the one ADT they name, and inserts the checked
unbox that `cast` would. Without it the scrutinee stays `any` and the emitter
takes its by-value-sum arm, which spells `T __scrut_v = (<tur_tagged_t>)`.

The rule's own comment predicted exactly that C -- "a compiled `match` reads a
tag out of a C aggregate, and `tur_adt_Lst __scrut_v = xs;` where `xs` is a
two-word box is 'invalid initializer'" -- and then gated the fix to the dialect
where the author had met it.

The gate was describing where the shape ARRIVES BY DEFAULT (in Saffron an
unannotated parameter is `any`), not where it applies. `any` is a Turmeric type
and `[l : any]` is a Turmeric signature, so the shape is writable in both and
only one of them worked.

## Fix

The dialect condition is gone; the narrow is keyed on the scrutinee's type
alone. Nothing in it was dialect-specific.

It stays CHECKED, which is the property worth not losing: handing it an `any`
holding something else panics with the standard cast message rather than
reinterpreting the payload.

```
panic: cast: any holds int, not Lst
```

Pinned by `tests/fixtures/match-on-any-plain-turmeric` (no `#lang` line): the
tag read, an arm BINDER read back out through the narrow, and a scrutinee whose
static type is already the ADT, which must keep taking the ordinary path.

## Note for whoever removes the next dialect gate

Removing this one immediately exposed a latent double free in plain Turmeric
(`any_expr_is_owned_temp`, see the sibling report) -- the shape had simply been
unreachable there because it did not compile. A dialect gate can be hiding a
bug rather than scoping a feature; run the leak-check gate over the newly
reachable shapes before assuming otherwise.
