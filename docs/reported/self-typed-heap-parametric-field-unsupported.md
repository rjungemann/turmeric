# A self-typed field on a `:heap` parametric struct is unsupported, and used to overflow the compiler

**Severity: medium** (was a compiler crash; now a clean rejection or ICE).
Found 2026-09-19 while assessing M7 of
[saffron-dynamic-surface-pass](saffron-dynamic-surface-pass.md): the fix that
report needs is exactly this shape -- `stdlib/list.tur`'s `Cons` with its tail
declared as the recursive occurrence instead of the erased `:int`.

## Repros

```turmeric
(defstruct Node :heap [A] (val A) (next (Node A)))

;; (1) an untyped terminator
(let [n (make-struct Node 7 (make-struct Node 8 0))] ...)
;; error [TUR-E0001]: function 'Node' arg 2: expected int, got int
;;   -- a self-contradictory diagnostic: both sides print as `int`

;; (2) a typed terminator
(let [m (make-struct Node 8 (:: 0 (Node int)))
      n (make-struct Node 7 m)] ...)
;; BEFORE 2026-09-19: AddressSanitizer: stack-overflow in tur itself
;;   (adt_app_is_byvalue_product recursing through the self-typed field)
;; AFTER:  tur: internal error (ICE): a representation decision disagrees
;;   with repr_of at binding.  repr-shadow binding let-bind
;;   type=(type-app Node int) want=heap-ptr got=carrier-i64
```

## What was fixed

`adt_app_is_byvalue_product` (types.c) re-entered itself on `(Node int)`
from inside `(Node int)` -- the field walk had no base case for a field
naming an application of the same def, because `is_self_recursive` is not
set for this shape and the SR2b exclusion keyed on it. It now keeps a stack of
the defs whose walk is in progress and answers false for one already on it (a
self-referential application cannot be inlined as a flat by-value product,
which is what the non-parametric recursive-field marking decides too). The
compiler no longer crashes; nothing else about the shape changed.

## What is still wrong

- (1) is rejected at the ctor call with a diagnostic whose two sides are the
  same word: the field's type prints as `int` (the erased carrier spelling of
  a `:heap` app in a ctor signature) while the argument is a real `int`, and
  they are not `type_eq`. The message should name `(Node A)`, and `0` should
  be accepted as the NULL link the way it is for the `:int` spelling.
- (2) reaches emit and trips the repr ratchet: the `let` binder wants the
  heap-pointer spelling for `(Node int)` and the init came out as the int64
  carrier. The representation of a self-typed `:heap` field (a typed pointer
  to the same record type) and the ctor / binder crossings around it are not
  wired.

## Why it matters

Both M7 (walking a cons list through an `any`) and the `& rest : any` gap in
the Saffron report reduce to declaring `Cons`'s tail as `(Cons A)`. Until this
shape is supported, neither can move.
