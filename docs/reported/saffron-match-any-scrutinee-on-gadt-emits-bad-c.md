---
title: "Saffron: `(match s ...)` on an `any` scrutinee whose ctor is a `defgadt` emits uncompilable C"
category: Reported
description: In a #lang saffron file, matching an unannotated (any) scrutinee works for a defdata constructor and fails to compile for a defgadt one -- the emitter casts the 16-byte tagged aggregate straight to a pointer. Annotating the parameter fixes it. Loud failure, not a wrong answer.
---

# Saffron `match` on an `any` scrutinee breaks for `defgadt` (but not `defdata`)

**Severity: medium.** It is a *loud* failure -- the emitted C does not compile,
so nothing miscomputes and nothing ships wrong. What it costs is the dialect's
headline property: in Saffron you should be able to leave a parameter
unannotated. Here you cannot, and the message you get is about C aggregates.

Found while building `tests/fixtures/saffron-static-guarantees-still-hold` for
saffron-lang-plan D7. That fixture works around it by annotating the scrutinee;
the workaround is called out in its header comment so this report and the
fixture do not drift apart.

## Repro

Three programs, one variable between them.

```turmeric
#lang saffron
(defgadt Shape [a]
  (Sq int : (Shape int)))
(defn area [s] (match s (Sq w) (* w w)))          ;; `s` is `any`
(defn main [] (println (cast (area (Sq 6)) int)) 0)
```

```
/tmp/tur-build/m-any-gadt_tur.c:7640:13: error: aggregate value used where an
integer was expected
 7640 |   tur_adt_Shape *__scrut = (tur_adt_Shape *)(intptr_t)(s);
      |   ^~~~~~~~~~~~~
tur: cc invocation failed (status 256)
```

Annotate the scrutinee and it works:

```turmeric
(defn area [s : Shape] (match s (Sq w) (* w w)))   ;; => 36
```

Use `defdata` instead of `defgadt`, leaving the scrutinee `any`, and it also
works:

```turmeric
(defdata Shape (Sq :int))
(defn area [s] (match s (Sq w) (* w w)))           ;; => 36
```

So the failure needs BOTH halves: an `any` scrutinee AND a GADT constructor.
Either one alone is fine.

## Root cause (partial)

`s` is `any`, so its C representation is `tur_tagged_t` -- a 16-byte
`{ tag, val }` aggregate, not a word. The GADT match arm emits the ordinary
concrete-scrutinee prologue,

```c
tur_adt_Shape *__scrut = (tur_adt_Shape *)(intptr_t)(s);
```

which casts the aggregate directly. The `defdata` path does not do this,
because `elab_match` has an `any`-scrutinee branch for Saffron
(`src/compiler/elab_structs.c:3730`, `scrutinee->type.kind == TY_ANY &&
lang_span_is_saffron(call->span)`) that unboxes first. The GADT path evidently
reaches the emitter without going through it.

I have not confirmed *where* the GADT path diverges -- whether it never
consults that branch, or consults it and is refused because the scrutinee type
is a GADT app. That is the next thing to look at, and it is the reason this is
filed rather than fixed: the fix is one of those two shapes and they want
different code.

## Fix directions

1. **Route the GADT match through the same `any`-scrutinee unbox** the
   `defdata` path uses. If the divergence is that the branch is simply not
   reached, this is small and is the consistent answer -- one unboxing seam for
   every ADT match, GADT or not.
2. **If the branch IS reached and declines**, the question is what it should
   unbox an `any` *to* when the target is a GADT app whose index is not yet
   known. A GADT match arm refines the index, so the unbox may need to happen
   per-arm rather than once in the prologue.

Direction 1 first, and only fall to 2 if the measurement says the branch is
already being consulted.

## Not this bug

The GADT machinery itself is fine in Saffron. Skolem escape still fires
(`tests/fixtures/errors/saffron-gadt-skolem-escape`), and an annotated GADT
match compiles and runs. This is about the `any` seam, not about GADTs losing
their refinement in the dialect -- which was measured and is not happening.
