---
title: An `: any` expectation does not drive widening at a `let` binding or an `if` join
category: Archive
description: RESOLVED 2026-09-07. Four positions, not the two filed -- `let`, `def`, the `if` join, and letrec/named-let forward declarations -- each with a distinct cause. Widening to `any` happens at a call argument, an `: any` return, and a branch facing an already-`any` sibling -- but not from the expectation itself. `(let [x : any 42] (type-of x))` leaves x typed int and type-of rejects it; `(defn f [b] : any (if b (Some 7.1) 42))` is "if branches have mismatched types" even though both would widen to the declared `any`. Two positions, one missing coercion.
---

# An `: any` expectation does not drive widening

**RESOLVED 2026-09-07 -- four positions, not the two filed.**

Fix direction 3 said to check whether other binding-shaped positions had the
same gap. They did, and each had a **different cause**, which is why one fix
would not have covered them:

| position | cause | symptom |
| --- | --- | --- |
| `let` | the binding takes the INITIALIZER's type; the annotation feeds only a mismatch check that skips non-primitives | annotation silently did nothing |
| `def` | hand-builds its `EX_ASCRIBE` instead of going through `elab_ascribe`, so it missed the coercion that does | `tur_tagged_t` slot assigned a `long int` -- a **cc error** |
| `if` join | nothing pushed a declared `: any` return in as the body's expected type (`body_expected` covered ADT/app/fn/exists/tyvar and stopped) | "if branches have mismatched types" |
| letrec / named `let` | `fwd_decl_scan_params` commits a forward param kind only from an allow-list, and `any` was missing | "function 'loop' arg 2: expected int, got any" |

The last one is the one the report did not anticipate at all, and it was found
by sweeping rather than by reasoning: `cstr` and `float` accumulators worked
while `any` did not, which localised it to that allow-list immediately. `any`
belongs there for the same reason `bool`/`cstr`/`nil` do -- it is spelled as a
bare symbol and fully determined by it. It is only "compound" in its
*representation*, which that pre-pass does not care about.

Two details worth keeping:

- The `let` coercion runs **after** the move/alias tracking, not before. Those
  inspect `init->kind == EX_VAR`, and widening earlier would wrap the init in
  `EX_UNION_INJECT` and silently switch both off for every `: any` binding.
- The `if` join widens only when the arms actually **disagree**. When they
  already share a type there is nothing to reconcile, and the return-position
  widen boxes the result once at the tail rather than once per arm -- so the
  change is inert for programs that were already fine.

**Verified** across all four positions and every payload shape (int, cstr,
float, struct), with the widen round-tripping back out through `cast` and
`is?` rather than merely relabelling; and the negative direction preserved --
an `if` NOT under an `any` expectation still reports mismatched branches.
Byte-identical compiled and interpreted.

**Fixture:** `any-coercion-from-annotation`. **Suites:** `run.sh` 2841/0,
`run-turi.sh` 1934/0, `check-examples` 38/0, `run-stdlib-checks` 35/0,
`run-leak-check` 83/0.

---

## The original report

**Severity: medium.** Two ordinary-looking programs are rejected for reasons
that read as type errors but are really a missing coercion. Both annotate `any`
explicitly, which is the one thing a user has to do to opt into the top type,
and neither gets it.

Found while fixing
[generic-fn-in-any-return-position-emits-uncompilable-c](generic-fn-in-any-return-position-emits-uncompilable-c.md).
Independent of that defect: the failures reproduce with a plain literal and no
generic in sight.

## Repro (2026-09-07)

**A `let` annotated `: any` does not widen its value.**

```turmeric
(defn main [] : int (let [x : any 42] (println (type-of x))) 0)
```
```
error: 'type-of' expects an 'any'-typed argument, got 'int'
```

`x` keeps the value's own type; the annotation is accepted and then does
nothing. Same for `"hi"`, a struct, and a non-generic call -- it is the
position, not the value:

| binding | result |
| --- | --- |
| `(let [x : any 42] ...)` | got `int` |
| `(let [x : any "hi"] ...)` | got `cstr` |
| `(let [x : any (make-struct P 1)] ...)` | got the struct type |
| `(let [x : any (g)] ...)` where `g : float` | got `float` |

**An `if` under an `: any` return does not widen mismatched branches.**

```turmeric
(defn f [b : bool] : any (if b (Some 7.1) 42))
```
```
error: if branches have mismatched types: then=(type-app Option float) else=int
```

Both branches would widen to the declared `any`, which is the whole point of
declaring it.

## What already works, and why that localises it

The union/intersection guide describes widening as happening "at a call
argument, a function's `: any` return position, or an `if` branch facing an
`any` sibling". That last clause is doing real work -- an `if` whose *other*
branch is already `any` widens fine:

```turmeric
(defn w [] : any 42)
(defn f [b : bool] : any (if b (Some 7.1) (w)))   ;; => "Option", works
```

So the coercion machinery is present and reachable at the join; what is missing
is treating the **expected type** as a widening target the way the return
position does. The `let` case is the same missing step at a binding.

## Fix directions

1. **Coerce at the binding when the annotation is `any`.** A `let` binding
   whose annotation resolves to `TY_ANY` and whose value is narrower should go
   through `elab_coerce_to_any`, exactly as the `: any` return position does.
   This is the smaller and less risky half.
2. **Coerce both `if` arms when the expected type is `any`.** At the branch
   join, before reporting a mismatch, check whether the expectation is `any`;
   if so widen both arms rather than requiring one of them to already be `any`.
   Ordering matters here -- the mismatch diagnostic currently fires first.
3. Consider whether the same gap exists at other binding-shaped positions that
   take an annotation (`loop`/`recur` accumulators, `def`). Not probed; the two
   above were found by accident rather than by a sweep.

Worth a fixture per position, asserting `type-of` round-trips on both back
ends -- these went unnoticed because no fixture annotates `: any` anywhere
except a function return.
