# Two classes declaring the same method name dispatch by declaration order, silently

**Severity: high** -- a **silent wrong answer**, with no diagnostic anywhere.
`tur check` exits 0 with zero output, cc is happy, and the program runs; it
just calls the other class's instance. Reordering two unrelated `definstance`
forms changes the answer.

**Status:** open. Found 2026-09-13 while validating the
[msgpack spice plan](../upcoming/hold/msgpack-spice-plan.md)'s class-naming
decision (does json's `Encode` really have to be renamed, or could msgpack
declare a method of the same name?). It has to be renamed -- this is why.

## Repro

```turmeric
(defclass EncodeJson [a] (encode [x] : cstr))
(defclass EncodeMp   [a] (encode [x] : int))    ;; same method name, other class

(definstance EncodeJson [int] (encode [x] : cstr
  ```c
  char *b = (char*)malloc(24); snprintf(b,24,"json:%lld",(long long)x); return b;
  ```))

(definstance EncodeMp [int] (encode [x] : int
  ```c
  return x + 1000;
  ```))

(defn main [] : int
  (println (encode 42))
  0)
```

```
$ tur check p.tur
$ echo $?
0                    # no diagnostic of any kind

$ tur run p.tur
1042                 # EncodeMp -- the SECOND definstance

# swap only the two definstance blocks, change nothing else:
$ tur run p.tur
json:42              # EncodeJson
```

Both classes are fully applied at `int`, both instances are legal on their
own, and the call site `(encode 42)` is ambiguous between them. Nothing says
so.

## Why it matters

Typeclasses resolve globally, so this is reachable the moment two libraries
each declare a class with an obviously-named method -- `encode`, `decode`,
`size`, `render`, `to-str`. The two serialization spices are the concrete
case: if `spices/msgpack` declared `encode` alongside `spices/json`'s
`Encode.encode`, a program importing both would serialize to whichever format
happened to register its instance last, and a later unrelated edit to import
order or file order could flip it.

The failure is worse than an ambiguity error in three ways: it is silent, it
is nonlocal (the two declarations may be in different libraries neither of
which is wrong on its own), and it is order-dependent, so it can survive
review and flip later.

## Root cause

Method-name resolution walks the instance registry and takes the first
name match, with no ambiguity check.

- `typeclass_env_find_method` (`src/compiler/typeclass.c:71`) scans
  `env->typeclasses` and returns on the first class with a matching method
  name. No second pass, so a second match is never seen.
- `elab_user_method_instance_matches` (`src/compiler/elab_call.c:2592`)
  likewise `break`s out of the method-name loop at the first hit, then
  returns `true` on the first instance whose type arg matches the receiver.
- Both walk singly-linked registries (`env->typeclasses`, `env->instances`)
  whose heads are the most recently registered entry
  (`typeclass.c:64`, `inst->next = env->instances`), which is what makes the
  outcome declaration-order-dependent.

There is an adjacent, intentional precedent that should NOT be swept up by a
fix: `TypeClass.from_stdlib` (`src/compiler/typeclass.h:120`) exists so that a
user `defn` may deliberately shadow a stdlib class method, and TUR-W0039 is
suppressed for exactly that case. That is defn-vs-method. This report is
method-vs-method across two *user* classes, which has no such story.

## Fix directions

Cheapest useful fix is a diagnostic, not a resolution-order change:

1. In `typeclass_env_find_method`, keep scanning after the first hit. If a
   second *different* class declares the same method name and neither is
   `from_stdlib`, emit a new `TUR-E####` (or `TUR-W####` if a hard error is
   too disruptive) at the call site naming both classes and telling the
   caller to disambiguate. A call is only genuinely ambiguous when both
   classes have an instance matching the receiver, so the check can be
   narrowed to that if the blunt version is too noisy.
2. Optionally warn at `defclass` time (`elab_typeclasses.c:1269`) when a newly
   registered class declares a method name an existing non-stdlib class
   already declares. This catches it earlier and at the declaration the author
   controls, but cannot distinguish the harmless case (the two classes are
   never used in one program) from the harmful one.

(1) is the one that matters; (2) is a nicety.

A qualified call syntax -- naming the class at the call site -- would be the
real expressiveness answer, but is a much larger change and is not needed to
close the silent-wrong-answer hole.

## Workaround

Give each class's methods a distinct name. This is what the msgpack plan now
does: `encode-json` / `encode-mp`, never a shared `encode`. See
[msgpack-spice-plan.md](../upcoming/hold/msgpack-spice-plan.md).
