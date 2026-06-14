# `ic_exec_accessor` silently miscompiles negated/disjunctive boolean returns

> **FIXED (TI8.b/W4):** `ic_exec_accessor` now **refuses** (returns `turi_nil`
> -> clean "inline-C not supported" error) any field-access return whose
> expression contains a result-transforming operator (`||`, `&&`, `==`, `!=`,
> unary `!`, `<`, `>`, with `->` skipped) instead of reading the bare field.
> `result-basic`'s `u-err?` and the minimal repro below now error cleanly rather
> than returning the inverted answer. Direction 2 (refuse) was chosen over
> direction 1 (evaluate) to stay safe; the `var ? var->f : fb` and
> `field ? field : "def"` shapes are unaffected (no such operators), and the
> allowlisted inline-C fixtures (`inline-c-binop`, `gen-*`) still pass. See
> `src/turi/eval.c` `ic_exec_accessor`.

**Summary:** The interpreter's simple inline-C evaluator
(`try_exec_simple_inline_c` -> `ic_exec_accessor`, `src/turi/eval.c:2114`)
mis-evaluates a `return` whose expression is a boolean combination over a struct
field -- specifically `return p == NULL || !p->field;`. It ignores the `== NULL`
null-check, the `||`, and the `!` negation, and returns the bare field value
(here `p->is_ok`) instead of its logical negation. The result is a **silent
wrong answer** (no error, no crash), which is the worst failure mode.

**Severity:** High (silent miscompile). The compiled path is correct; only the
`--interpret` path is wrong, so a program can pass when built and silently
return garbage when interpreted.

## Minimal repro

```turmeric
(defn mk [] : ptr<void>
  ```c
  struct { bool is_ok; int64_t v; } *r = malloc(sizeof(*r));
  r->is_ok = true; r->v = 5; return r;
  ```)
(defn is-err? [r : ptr<void>] : bool
  ```c
  struct { bool is_ok; int64_t v; } *p = (void*)r;
  return p == NULL || !p->is_ok;
  ```)
(defn main [] : int
  (let [r (mk)]
    (println (is-err? r))   ; prints "true"; correct answer is "false"
    0))
```

```sh
ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret /tmp/ic.tur
# => true   (WRONG -- r is ok, so is-err? must be false)
```

`tests/fixtures/result-basic/` is the in-tree case: `(u-err? r1)` (r1 ok) prints
`true` instead of `false`, and `(u-err? r2)` (r2 err) prints `false` instead of
`true` -- the predicate is consistently inverted. The fixture is inline-C-bound
(every helper is a ` ```c ` block), so it would ultimately be a TI7/W2 carve-out
-- but it must NOT be silently carved while the interpreter is returning wrong
answers (CLAUDE.md: a test that surfaces real broken behavior is a finding).

## Root cause

`ic_exec_accessor` is a pattern-matcher over the `return` expression. It handles:

- `return var ? var->field : fallback;` (ternary with null-check), and
- `return var->field;` / `return var.field;` (plain field access), and
- apparently `return var != NULL && var->field;` (the `u-ok?` shape returns
  correctly).

But for `return var == NULL || !var->field;` it falls through to the plain
field-access branch (after skipping the `var` identifier it does not account for
the `==`/`||`/`!` operators) and returns `var->field` verbatim -- dropping the
negation and the disjunction.

## Why a blanket guard is not the fix

The obvious "refuse any logical-operator return" guard would also refuse the
`!= NULL && ->field` shape (`u-ok?`), which currently evaluates **correctly** --
so blanket refusal would turn a passing accessor into a clean error and could
regress fixtures that rely on it. The fix must either:

1. **Evaluate the boolean expression correctly** -- extend `ic_exec_accessor`
   (or a small sub-evaluator) to handle `==`/`!=`/`!`/`&&`/`||` over
   `ptr == NULL` and `ptr->field`, with C short-circuit semantics; or
2. **Refuse precisely the mis-handled shapes** -- detect that the return
   contains `!` or `||`/`==` that the current code does not fold, and return
   `turi_nil()` (-> clean "inline-C not supported" error) only for those, while
   leaving the proven-correct `!= NULL && ->field` path intact.

Direction 1 is more work but recovers the fixtures; direction 2 is safer and
converts the silent miscompile into a clean carve-out. Either way, audit the
other `ic_exec_*` pattern matchers (`ic_exec_str_cmp`, `ic_exec_constructor`,
`ic_exec_snprintf_fmt`) for the same "match loosely, evaluate wrongly" hazard.

## Validation

After a fix, the repro prints `false`; `result-basic` either prints the correct
`true/false/false/true ... 99 ...` sequence (direction 1) or fails with a clean
`inline-C not supported` error (direction 2, then carve it `requires.tur-only`).

## Status

Filed while investigating TI8.b/W1b. This is a W4 (silent-miscompile) item in
[docs/archive/history/turi-interpreter-gap-closure-plan.md](turi-interpreter-gap-closure-plan.md);
it is independent of the native-shim reconciliation and can be fixed on its own.
