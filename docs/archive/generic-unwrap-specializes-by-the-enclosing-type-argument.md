# Inside a generic, `unwrap` is specialized by the enclosing type argument, not by its own argument

**RESOLVED 2026-09-14** by fix direction 1. The report's closing hint was the
answer: the binding set **is** consulted by name, and this is name capture.

stdlib's `unwrap` is `(defn unwrap [A] [o : (Option A)] :A ...)`. Its type
parameter is called `A`, and so is the enclosing generic's. `emit_abi_find_type_binding`
matches on `strcmp(name)`, so inside `firstn__spec__double` the callee's own `A`
resolved out of the caller's `{A -> float}` -- which is also why renaming the
enclosing generic's parameter to `B` is a complete workaround (confirmed as a
control while fixing this: the identical program compiles and runs).

`emit_abi_register_call` now unifies the callee's declared parameter types
against the **concrete argument types at the call site**, and a binding
recovered that way overrides the inherited one. Four conditions keep it to the
shape this report names, and each was measured against a fixture that regressed
without it:

- **Not a typeclass method** (no `dict_arg`, no `owner_instance`). An instance
  dispatch resolves its class var from the RECEIVER and the dispatch paths need
  that binding verbatim -- correcting it from the argument reroutes
  `Enc[Option]` calls to the wrong element clone.
- **The parameter pins its tyvar inside a type application** (`o : (Option A)`),
  so the argument's own head type determines it. A bare-tyvar parameter
  (`x : A`) is whatever the caller says it is, which is exactly what the
  instance paths resolve from the receiver.
- **That application's spine head is a concrete constructor**, not a
  higher-kinded type variable. A `(m a)` parameter binds its head to a partial
  application that may carry a HOLE -- `(Result _ cstr)` saturates at position
  0, not on the end -- and `emit_abi_unify_collect` walks `fn`/`arg`
  positionally with no hole handling, so it would transpose `(Result int cstr)`
  into `m -> (Result int)`, `a -> cstr`. `emit_abi_instantiate_type` carries the
  hole logic; this unifier does not, so it must not be asked the question
  (`tests/fixtures/hkt-constrained-hole-headed-instance-head`).
- **The argument's type is concrete as written**, not concrete only after being
  resolved through the active spec. An argument that mentions the enclosing
  generic's tyvars genuinely depends on it; this correction is only for one
  whose type has nothing to do with the enclosing type parameter -- the shape
  the report names, and the only one a name collision can misresolve.

A parameter pattern that does not line up with its argument (a `(Option A)`
against a carrier-collapsed `int64`) collects nothing, so the pass can only ever
narrow.

The capture has a **second site, on the result**, and correcting only the
arguments is not enough. `unwrap`'s declared result is its own `A`, and a
recovery further down `emit_abi_register_call` re-resolves a bare-tyvar result
through the ACTIVE SPEC's bindings -- by name again. That is right when only
the spec knows the element and wrong when the call site has already pinned it,
so it undid the argument fix and left the clone
`unwrap__spec__double_tur_adt_Option__int`: a `double` landing in the `int64`
slot the concrete `(Option int)` calls for. That shape is worse than the
original cc failure, not better -- it compiles, and it prints the right answer
for a small integer while waiting for a payload that does not survive the
truncation. It was caught by the suite's `-Wfloat-conversion` ratchet (which is
NOT in the default cc flags, so a hand-run `tur build` of the fixture looks
clean). The result is now derived from the corrected bindings, and that
recovery stands down when it has been.

Pinned by `tests/fixtures/generic-unwrap-unrelated-option`. The **two**
instantiations are load-bearing, as the report says: at `A = int` alone the
fixture passes with the bug present.

## Original report

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
