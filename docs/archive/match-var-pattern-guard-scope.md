# A var pattern's binder is not in scope for its own `when` guard (RESOLVED)

**Severity:** medium -- a legal-looking program failed to compile. No soundness
implication; the guard simply could not see the name the pattern had just
bound.

## Repro

```turmeric
(defn f [p : int] : int
  (match p
    x when (> x 2) x
    _              0))

(defn main [] : int (println (f 7)) 0)
```

```
$ ./build/tur build repro.tur
repro.tur:3:15: error [TUR-E0003]: unbound symbol 'x'
repro.tur:3:15: help: Did you mean 'p'?
```

## Scope

ELABORATION, not codegen -- `TUR-E0003` is raised before any lowering runs.
Specific to a var pattern on a scalar (non-ADT) scrutinee. A constructor
pattern's binder was always fine (`tests/fixtures/gadt-guard` covers it), and a
wildcard or literal pattern binds nothing, so for those the ordering was
unobservable:

```turmeric
(match s (Pos n) when (> n 100) ...)   ; ok -- ADT path
(match p _       when (> p 2)   ...)   ; ok -- binds nothing
(match p x       when (> x 2)   ...)   ; unbound symbol 'x'
```

Exactly the binder-plus-guard combination on the scalar path.

## Root cause

`elab_match` in `src/compiler/elab_structs.c` has three arm-elaboration paths
-- union, scalar (`_is_prim`), and ADT. The scalar path elaborated the guard
*before* creating the arm scope:

```c
/* Optional when-guard */
if (guard_raw) {
    lit_arms[ai].guard = elab_form(e, guard_raw);   /* <-- no arm scope yet */
    ...
}

/* Elaborate body; for is_var, introduce the binding in a new scope */
if (pat->is_var && pat->var_sym) {
    Binding *vb = binding_new(e, pat->var_sym, scrutinee->type, ...);
    scope_init(&arm_sc, e->scope);
    ...
}
```

The ADT path had it right, and said so in a comment -- "Elaborate optional
when-guard **while arm scope is still live**" (`elab_structs.c`, the
`arm_has_guard[ai] && guard_form_raw` block). The scalar path had no
equivalent step because it had no arm scope at the point the guard was
elaborated.

## Fix

Move guard elaboration inside the arm scope on the scalar path, so both
branches (binder / no binder) elaborate guard and body in the same scope the
body sees. A second, smaller gap closed with it: the scalar path never checked
the guard's *type*, so an `:int` guard was silently accepted on one path and
rejected on the other. The ADT path's bool check is now mirrored, which is
reachable only because the guard is elaborated in a place that has a type to
check.

## Coverage

- `tests/fixtures/match-var-pattern-guard-scope` -- fifteen assertions across
  eleven shapes: guard passing and failing through, the binder used on both
  sides, two guarded var arms in sequence, a literal arm ahead of a guarded var
  arm, an arm binder shadowing an outer parameter, nested matches whose inner
  binder shadows the outer one, a guarded arm with no following arm, a guard
  calling an ordinary function, and float and cstr scrutinees.
- `tests/fixtures/errors/match-scalar-guard-not-bool` -- pins the new bool
  check on the scalar path.
- `tests/refine-fuzz-src.py` -- `shape_datatype` gained five var-binder-guard
  rungs. They had never been generated because they could not compile; they now
  fire in roughly 29% of that shape's samples. They are the sharpest guard rung
  available, because the guard is written about the *binder* while any
  synthesized hypothesis is about the *scrutinee* -- two of the five make those
  two names disagree on purpose (`(match (- p p) x when ...)` and a `let` that
  rebinds `x` outside the match), so reading the guard as a constraint on `p`
  would assert something false.

## Verification

Suite 2332 passed / 0 failed. Solver units 2/2. 400 fuzz cases over seeds
2201/2202 plus the fuzzer self-test: 0 soundness bugs, 0 other BUG classes. The
9 report-only `SUSPICIOUS_over_refute` cases were inspected -- none is a
var-binder-guard shape, and they are the known limitation of a run-based
oracle: refutation is a claim about all inputs, while the oracle observes one
execution.

## Found

Alongside two codegen defects in the same construct, filed and fixed
separately -- see `docs/archive/match-scalar-wildcard-and-guard-codegen.md`.
Those two were lowering bugs resolved by rewriting the scalar-match block; this
one never reached lowering, so it was a separate job in a separate file.
