# Nested constructor patterns in `match` are unsupported

**Summary:** `match` rejects a constructor pattern whose field is itself a
constructor pattern (e.g. `(Right (Left a))`). Each field binding must be a
bare symbol, so nested sums must be destructured in two manual steps.

**Severity:** Ergonomics / expressiveness gap. Not a miscompile -- it is a hard,
well-located parse-time/elaboration error, and a clean two-step workaround
exists. It blocks task T5.4 of `docs/upcoming/sum-types-either-plan.md`
("Nested patterns: `(Left (Just x))` decomposes through nested sums").

## Minimal repro

```turmeric
(defdata Either [L R] (Left L) (Right R))

(defn deep [e : (Either int (Either int int))] : int
  (match e
    (Left n)          n
    (Right (Left a))  a              ; <-- nested constructor pattern
    (Right (Right b)) b))
```

### Observed

```
error: match: field binding must be a symbol
    (Right (Left a))  a
           ^^^^^^^^
```

### Expected

The nested pattern binds `a` (resp. `b`) to the inner Left/Right payload, with
the inner arm types in scope for the body -- the same decomposition Rust/ML
`match` performs.

## Root cause

`src/compiler/elab_structs.c`, in `elab_match` (around line 2658-2668): when an
ADT arm's fields are bound, each field form is required to be `F_SYM`:

```c
for (uint32_t bi = 0; bi < n_bindings; bi++) {
    Form *var_form = pat_form->as.list.items[1 + bi];
    if (var_form->tag != F_SYM) {
        diag_emit(DIAG_ERROR, var_form->span,
                  "match: field binding must be a symbol");
        ...
    }
    ...
}
```

A field that is an `F_LIST` (a sub-pattern) is rejected outright rather than
recursively elaborated. The codegen side (`emit_expr.c`, `EX_MATCH`) likewise
only emits a single level of `__scrut->as.<Ctor>._N` field extraction.

## Proposed fix directions

1. **Recursive pattern elaboration.** Generalise the field loop: when a field
   form is a constructor pattern, allocate a fresh temp binding for the field,
   then recurse to elaborate the sub-pattern against the field's type, threading
   the resulting bindings into the arm scope. Exhaustiveness/coverage tracking
   must also recurse (a nested match is exhaustive only if every reachable inner
   constructor combination is covered or a wildcard is present).
2. **Codegen.** Emit nested `if (tag == ...)` / field-extraction for each level,
   reusing the existing single-level destructuring per nesting depth.
3. **Scope.** Keep the desugaring equivalent to the manual two-step form below,
   so behaviour (and the linear consumption of the scrutinee) is unchanged.

## Workaround (current idiom)

Bind the inner sum to a symbol, then match it:

```turmeric
(match e
  (Left n)      n
  (Right inner) (match inner
                  (Left a)  a
                  (Right b) b))
```

This is exercised by `tests/fixtures/sum-either-nested/`.

## How to validate a fix

- The minimal repro above should compile and run, returning the inner payload.
- Add a fixture `tests/fixtures/match-nested-ctor/` mirroring
  `sum-either-nested` but using the sugar, with identical `expected.stdout`.
- A non-exhaustive nested match (e.g. omitting `(Right (Right b))`) must still
  be a hard error citing the missing inner constructor.
