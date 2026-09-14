# Inside a generic, `unwrap` is specialized by the enclosing type argument, not by its own argument

**Severity: medium-high** -- a **hard cc failure**, and one that hides: the
program compiles for as long as the generic is only ever instantiated at the
element type the unrelated `Option` happens to hold. The second instantiation
is what breaks it, and nothing points at the first.

**Status:** open. Found 2026-09-14 building `spices/msgpack`, where
`decode-mp-list` worked at `(Cons int)` through several rounds of testing and
failed the moment a `(Cons float)` call site was added.

## Repro

A type-parameterized `defn` that calls `unwrap` on an `(Option int)` -- a value
whose type has nothing to do with the type parameter:

```turmeric
(defn an-option-int [] : (Option int)
  (:: (some 7) (Option int)))

(defclass Mk [a] (mk [n : int] : a))
(definstance Mk [int]   (mk [n] : int   n))
(definstance Mk [float] (mk [n] : float 1.5))

;; `count` is an (Option int) in EVERY instantiation of firstn.
(defn firstn [A] [(Mk A)] [n : int] : A
  (let [count (unwrap (an-option-int))]
    (mk (+ n count))))

(defn main [] : int
  (println (:: (firstn 1) :int))
  (println (:: (firstn 1) :float))   ;; <-- adding this line breaks the build
  0)
```

```
$ tur run p.tur          # with only the :int line
8

$ tur run p.tur          # with both lines
p_tur.c:8226:75: error: passing 'tur_adt_Option__int'
    (aka 'struct tur_adt_Option__int') to parameter of incompatible type
    'tur_adt_Option__float' (aka 'struct tur_adt_Option__float')
1 error generated.
```

At `A = float` the emitted call is

```c
unwrap__spec__double_tur_adt_Option__float(__ps_625)
```

against an argument of type `tur_adt_Option__int`. The specialization was
chosen from the enclosing generic's type argument (`A = float`) instead of from
`unwrap`'s own argument type (`(Option int)`, concrete and unambiguous at every
instantiation).

## Why it hides

At `A = int` the wrong answer and the right answer coincide -- `Option int` is
`Option int` -- so the generic compiles, runs, and passes its tests. In the
msgpack spice this held across the whole int-only phase of the work; the
failure arrived with the first `(Cons float)` and `(Cons cstr)` round-trip
case, in code that had not been touched. A reader looking at the diff sees a
new test and a broken list decoder, with no connection between them.

The same shape appears with any generic helper whose signature mentions a type
variable: `unwrap` is just the one reached first.

## Root cause

`emit_abi_instantiate_type` (`src/compiler/emit_module.c:2479`) instantiates a
callee's signature against the binding set carried on the call
(`call_.abi_bindings`, see the comment at `:2453`). For a call *inside* a
generic, that set is the enclosing function's -- `A := float` -- and `unwrap`'s
own `T` is resolved from it rather than from the concrete argument type.
`emit_abi_unify_collect` (`:2608`) is the unifier that should have bound
`T := int` from the argument and did not get the chance.

The scope of the bug is "a type variable named the same as, or resolved
through, the enclosing generic's" -- worth checking whether the binding set is
being consulted by *name* where it should be matching by *position in the
callee's own signature*.

## Fix directions

1. Unify the callee's parameter types against the **concrete argument types at
   the call** first, and consult the enclosing binding set only for type
   variables still unbound after that. A concrete argument should always win
   over an inherited binding.
2. Add a fixture: a generic `defn` that calls `unwrap` (or any parametric
   helper) on a value whose element type is unrelated to the type parameter,
   instantiated at two different type arguments in the same program. The
   two-instantiation part is load-bearing -- a single-instantiation fixture
   passes while the bug is present.

## Workaround in use, and what to remove when this is fixed

`spices/msgpack/src/msgpack/encode.tur` carries two concrete helpers whose only
job is to keep `unwrap` out of a generic body:

```turmeric
(defn __mp-arr-len [tree : MpTree node : MpNode] : int
  (let [o (mp-arr-size tree node)]
    (if (some? o) (unwrap o) 0)))

(defn __mp-arr-node [tree : MpTree node : MpNode i : int] : MpNode
  (let [o (mp-arr-get tree node i)]
    (if (some? o) (unwrap o) (:: -1 MpNode))))
```

`__mp-arr-decode` and `decode-mp-list` call those instead of writing
`(unwrap (mp-arr-get tree arr i))` and `(unwrap (mp-arr-size tree node))`
inline, which is what they said before this bug.

**When this report is resolved**, inline both back into their two call sites,
delete the helpers and the ~10-line comment above `__mp-arr-len` that explains
the hazard, and keep `spices/msgpack/tests/container-round-trip.tur` green --
its `(Cons int)` / `(Cons float)` / `(Cons cstr)` round-trip cases are exactly
the multi-instantiation coverage that catches a regression.
