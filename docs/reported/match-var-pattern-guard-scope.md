# A var pattern's binder is not in scope for its own `when` guard

**Severity:** medium -- a legal-looking program fails to compile. No soundness
implication; the guard simply cannot see the name the pattern just bound.

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
repro.tur:2:22: error: unbound symbol 'x'
```

## Scope

It is ELABORATION, not codegen -- the error is `TUR-E0003`, raised before any
lowering runs. Specific to a var pattern on a scalar scrutinee; a constructor
pattern's binder is fine, which `tests/fixtures/gadt-guard` covers:

```turmeric
(match s (Pos n) when (> n 100) ... )   ; ok
(match p x       when (> x 2)   ... )   ; unbound symbol 'x'
```

A wildcard with a guard is also fine (nothing is bound), and a literal with a
guard is fine. It is exactly the binding-plus-guard combination on the scalar
path.

## Root cause

Not pinned down. The guard appears to be elaborated in the enclosing scope
rather than in the arm's scope, so the pattern's binding has not been
introduced yet. The ADT match path must already introduce arm bindings before
elaborating the guard; the scalar path likely does not have the equivalent
step.

## Fix directions

Introduce the var pattern's binding into the arm scope before elaborating the
guard, mirroring whatever the constructor-pattern path does. Worth checking
whether the two paths can share that step rather than each having their own.

## Found

Alongside two codegen defects in the same construct, both since fixed --
see `docs/archive/match-scalar-wildcard-and-guard-codegen.md`. Those two are
lowering bugs and were resolved by rewriting the scalar-match block; this one
never reaches lowering, so it is a separate job.
