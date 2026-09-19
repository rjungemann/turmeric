# A self-typed field on a `:heap` parametric struct is unsupported, and used to overflow the compiler

**RESOLVED 2026-09-19.** The shape compiles end to end on both back ends.
Pinned by `tests/fixtures/heap-parametric-self-typed-field` (typed
terminator, nested ctor argument, generic `push` at two element types,
monomorphic and polymorphic recursive walks, `match`, an erased-identity and
vec-element round trip), `heap-parametric-self-typed-field-option-link`
(the link behind an `Option`), `saffron-heap-parametric-self-typed-field`
(`(Node any)`), and `errors/heap-parametric-self-typed-field-int-terminator`
(the corrected diagnostic).

**Severity: medium** (was a compiler crash; then a clean rejection or ICE).
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

## Resolution

Five defects sat in a row behind the one shape; each fix uncovered the next.

1. **Compiler stack overflow** -- `adt_app_is_byvalue_product` (types.c)
   re-entered itself on `(Node int)` from inside `(Node int)`: the field walk
   had no base case for a field naming an application of the same def. It
   now keeps a stack of the defs whose walk is in progress and answers false
   for one already on it.
2. **Self-contradictory diagnostic** -- the ctor mismatch report
   (`elab_call.c`) printed the param's carrier kind (`int`) when its full
   type was a `TY_APP`; it now prefers the full type, so (1) reads
   `expected (type-app Node tyvar 'A'), got int`. A bare `0` stays rejected:
   the typed terminator is `(:: 0 (Node int))`. Accepting `0` for a typed
   pointer would be the `:int` stand-in the type was declared to retire.
3. **Repr ratchet at the binder** -- `adt_app_is_byvalue_product_inner`'s
   SR2b field acceptance excluded every `is_self_recursive` def, so
   `(Node int)` fell to the int64 carrier while `repr_of` said typed pointer.
   A `:heap` def's self-reference is a pointer word in the record (the
   monomorph IS a typed pointer to its heap header), so it inlines no typedef
   into itself and the exclusion is now `(!is_self_recursive || is_heap)`.
   A non-heap self-typed def keeps the exclusion.
4. **Registration re-entry** -- `type_register_adt_app` recorded the ctor
   signatures BEFORE inserting the registry entry; recording c-names every
   field, and the self field c-names to the app being registered, so the
   lookup re-entered `record_adt_app_ctor_sigs` with no entry to short-circuit
   on (ASan DEADLYSIGNAL). The entry is now inserted first and flagged
   `recording` while its signatures are recorded; a re-entrant lookup returns
   the name. `emit_sig_record_param_ctype` (emit_module.c) also freed the
   string a ctor-call emitter was still holding when the re-record happened
   mid-spelling (heap-use-after-free in `emit_value_dispatch` for a generic
   `push`); it now keeps an equal string's pointer live and retires a
   replaced one, the discipline the return-type slot already had.
5. **Typedef ordering** -- the registered-app emitter's dependency pre-pass
   recursed into every app-typed field. For the direct self-reference that
   put `tur_adt_Node__int * next;` inside the typedef introducing the name;
   for `(next (Option (Node A)))` it broke the Option/Node cycle at the wrong
   point and emitted Node's body (which holds Option BY VALUE) before Option
   was complete. A dependency held by pointer -- any `:heap` monomorph -- now
   gets a guarded forward `typedef struct X X;` instead of a recursive emit,
   so the by-value holder always follows the pointer holder.

## Why it mattered

Both M7 (walking a cons list through an `any`) and the `& rest : any` gap in
the Saffron report reduce to declaring `Cons`'s tail as `(Cons A)`. The shape
is supported now, and the stdlib redeclaration landed later the same day
(M7 and `& rest : any` in the Saffron report are resolved).

## Found on the way, not fixed here

A nullary generic call as a ctor argument whose expected type is
tyvar-shaped -- `(make-struct W 8 (none))` for `(defstruct W :heap [A] (val
A) (opt (Option A)))`, or `(push 3 (node-nil))` -- is rejected with
`expected (type-app Option tyvar 'A'), got (type-app Option tyvar 'A')`. It
reproduces on a non-recursive def, so it is not this shape's; filed as
[nullary-generic-call-under-tyvar-expectation](nullary-generic-call-under-tyvar-expectation.md)
(resolved later the same day).
