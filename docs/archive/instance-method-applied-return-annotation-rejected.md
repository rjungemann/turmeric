# An instance method could not restate an applied result type

**Severity: medium.** An expressiveness hole with a misleading diagnostic:

```turmeric
(defclass Co [a] (co [x : a] : (Option a)))
(definstance Co [float] (co [x : float] : (Option float) (some x)))
;; error: type annotation ': type' is only valid after a parameter name or as
;;        a return type
```

The same form is accepted on the class declaration and on a method
parameter. Found 2026-10-01 while widening the type fuzzer's rank-2 class
crossing. **RESOLVED 2026-10-01.**

## Mechanism

`elab_definstance_inner` (`elab_typeclasses.c`) took an impl's return
annotation from a bare symbol (`: float`, `: Point`) or a `#refine{...}`.
Anything else, including a parenthesised type, was left as the first body form.
It then elaborated as a stray `F_TYPE_ANN` expression.

## Fix

A one-element `F_TYPE_ANN` holding any other type form is resolved through
`type_expr_from_form`, the way a parameter annotation is. An unresolvable one
is reported as such instead of falling into the body.

## Verified

`tests/fixtures/instance-method-applied-return-annotation` (an `(Option float)`,
an `(Option cstr)` and a `(Vec int)` result), compiled and `--interpret`.
