# Compound-Type Annotations Plan

**Status:** Planned

**Last Updated:** 2026-05-14

---

## Summary

Turmeric currently supports only simple keyword-style type annotations like `:int`, `:bool`, and `:ptr<void>`. This plan adds support for compound-type annotations written as `: type-expr`, where `type-expr` is any form — a symbol, a keyword, or a list expression such as `(-> a b)` or `(forall [a] T)`.

The preferred syntax is **space-separated**: `: type-expr` rather than the fused `:type-expr`. This simplifies the reader (`:` becomes a distinct marker rather than an atomic keyword prefix) and makes complex types legible.

The primary motivating use-case is `stdlib/arrow.tur`, which uses:

```lisp
(arr [a b] : (-> a b) (arr a b))
(>>> [a b c] : (arr a b) : (arr b c) : (arr a c))
```

The file is currently quarantined in `tests/wip/` because the reader errors on `:(` and because `|` is not in the symbol alphabet (blocking `|||`). Both issues are addressed by this plan.

---

## Preferred Syntax

Type annotations use a leading `: ` (colon + space) before the type expression:

```lisp
;; Simple type (replaces :int)
(defn add [a : int b : int] : int ...)

;; Function type
(defn apply [f : (-> int int) x : int] : int ...)

;; Parameterised type
(defn map-vec [v : (vec int) f : (-> int int)] : (vec int) ...)

;; Higher-ranked / polymorphic
(defn id [a b] : (forall [a] (-> a a)) ...)
```

Both `: sym` and `: (list ...)` are covered by the same reader path. The old `:keyword` syntax (no space) remains supported for backward compatibility throughout the migration.

---

## Motivation

### Current Limitation

`read_keyword()` in `src/reader.c` (line 446) checks:

```c
if (!is_sym_cont(peek(r)) && !isalpha(peek(r))) {
    diag_emit(DIAG_ERROR, s, "expected keyword name after ':'");
    r->error = true;
    return NULL;
}
```

When the next character after `:` is a space or `(`, parsing fails. This makes it impossible to write compound types at annotation sites.

### Symbol Alphabet Gap

`|` is missing from `is_sym_start` / `is_sym_cont` in `src/reader.c` (lines 140–164). The `stdlib/arrow.tur` file uses `|||` (parallel composition operator), which fails to lex.

---

## Design

### New Form Tag: `F_TYPE_ANN`

Add a new form tag in `src/forms.h`:

```c
F_TYPE_ANN,   /* `: type-expr` — type annotation wrapper produced by reader */
```

The form's `as.unary.inner` field holds the type expression form (a sym, keyword, or list). The span covers from `:` through the end of the inner form.

This wrapper makes it easy to distinguish "this form was written as a type annotation" from an ordinary list or symbol, and lets every annotation site in the elaborator handle both `F_KEYWORD` (backward compat) and `F_TYPE_ANN` (new path) uniformly.

### Reader Changes (`src/reader.c`)

#### 1. `read_keyword()` — three cases

```
peek after ':' is a valid sym char  →  existing F_KEYWORD path (unchanged)
peek after ':' is ':'               →  existing '::' symbol path (unchanged)
peek after ':' is ' ', '(', EOF,
  or any non-sym char except ':'    →  new F_TYPE_ANN path (described below)
```

New path:

1. Consume `:`.
2. Skip whitespace (`skip_ws_and_comments(r)`).
3. If `peek(r) == -1`, emit a diagnostic "expected type expression after ':'".
4. Otherwise call `read_form(r)` to obtain the inner type form.
5. Return `form_type_ann(r->arena, span, inner)`.

This handles both `: int` (space before sym) and `:(-> a b)` (paren immediately after `:`) via the same code path.

#### 2. `is_sym_start` / `is_sym_cont` — add `|`

```c
/* in is_sym_start switch */
case '|':
    return true;

/* in is_sym_cont switch */
case '|':
    return true;
```

This allows `|||`, `|>`, and similar pipe-shaped operators.

### Forms Layer (`src/forms.h`, `src/forms.c`)

Add `F_TYPE_ANN` to the `FormTag` enum and add a constructor:

```c
Form *form_type_ann(Arena *a, Span span, Form *inner);
```

Store the inner form in the existing `as.unary.inner` slot (already used by quote/unquote wrappers), or add a dedicated `as.type_ann` field if that is cleaner. The `form_print` / debug printer should render it as `: <inner>`.

### Elaborator Changes (`src/elab.c`)

#### New helper: `elab_type_from_form()`

A single function that accepts any `Form *` at a type-annotation position and returns `Type *`:

```c
static Type *elab_type_from_form(Elab *e, const Form *form);
```

Dispatch table:

| Form tag | Inner content | Result |
|---|---|---|
| `F_KEYWORD` | `"int"`, `"bool"`, `"cstr"`, … | existing primitive lookup |
| `F_TYPE_ANN` | inner sym | delegate to `elab_type_from_form(inner)` |
| `F_TYPE_ANN` | inner list | dispatch on head sym (see below) |
| `F_SYM` | type variable name | generic type variable (TY_UNKNOWN + name) |
| `F_LIST` | `(-> a b)` | TY_FN |
| `F_LIST` | `(forall [vs] T)` | TY_FORALL (already handled in `elab.c:9740`) |
| `F_LIST` | `(exists [vs] T)` | TY_EXISTS |
| `F_LIST` | `(vec T)`, `(option T)`, … | TY_APP |

List form dispatch (head symbol):

```
->       (-> arg-type... ret-type)  →  TY_FN
forall   (forall [vars] body)       →  TY_FORALL  (promote existing code)
exists   (exists [vars] body)       →  TY_EXISTS
vec      (vec T)                    →  TY_APP(vec, T)
option   (option T)                 →  TY_APP(option, T)
either   (either A B)               →  TY_APP(either, A, B)
tuple    (tuple A B ...)            →  TY_APP(tuple, A, B, ...)
rc       (rc T)                     →  TY_RC with inner = T
ref      (ref T)                    →  TY_REF with inner = T
arr      (arr A B)                  →  TY_APP(arr, A, B)  (HKT)
```

`(->)` desugars to TY_FN with the last argument as the return type and all preceding arguments as parameter types. Arity ≥ 1 required; arity 1 means `(-> T)` = nullary function returning T.

#### Annotation sites to update

All places in `elab.c` that currently check `p->tag == F_KEYWORD` for type annotations must also accept `p->tag == F_TYPE_ANN`. The common call sites are:

| Location | Approximate line | Context |
|---|---|---|
| `elab_defn` param loop | ~5895–5910 | `p->tag == F_KEYWORD` after param sym |
| `elab_defn` return type | ~5997 | `ret_f->tag == F_KEYWORD` |
| `elab_fn` param loop | ~6273 | same pattern |
| `elab_fn` return type | ~6298 | same pattern |
| `elab_extern_c` | ~6549–6604 | param and return type |
| `elab_defeffect` | ~5036–5059 | method param/return types |
| `elab_defclass` methods | ~9032–9116 | method signatures |
| `elab_field_type` helper | ~8500 | struct field types |
| `elab_type_app` | ~10175, 10219 | HKT type arg parsing |

The cleanest refactor is: introduce `elab_type_from_form()` and replace all inline keyword-dispatch blocks with a call to it, then add `F_TYPE_ANN` handling only once inside that function.

### Formatter Changes (`src/fmt.c`)

Add `F_TYPE_ANN` handling in both `fmt_form_flat()` and `fmt_form()`:

```c
case F_TYPE_ANN:
    buf_putc(b, ':');
    buf_putc(b, ' ');
    fmt_form_flat(b, form->as.type_ann.inner, src);
    break;
```

For the break/indent path in `fmt_form()`, a type annotation should always be printed inline (`: (-> a b)` is never broken across lines on its own). If the outer context needs to break, it does so at a higher level.

---

## Implementation Steps

### Step 1 — `|` in symbol alphabet

In `src/reader.c`, add `case '|':` to both `is_sym_start` (line ~144) and `is_sym_cont` (line ~160). This is a one-line-per-function change with no other dependencies.

Verify: `|||` and `|>` should lex as symbols.

### Step 2 — `F_TYPE_ANN` form node

1. Add `F_TYPE_ANN` to `FormTag` in `src/forms.h`.
2. Add `form_type_ann()` constructor in `src/forms.c` (mirror `form_unquote` / `form_quote`).
3. Add a case for `F_TYPE_ANN` in `form_print()` / any switch over `FormTag` that is exhaustive.

### Step 3 — Reader: `: type-expr` path

Modify `read_keyword()` in `src/reader.c`:

```c
/* New: ': ' or ':(' — compound / space-separated type annotation */
if (peek(r) == ' ' || peek(r) == '\t' || peek(r) == '\n' || peek(r) == '\r'
    || peek(r) == '(' || peek(r) == '[' || peek(r) == -1) {
    skip_ws_and_comments(r);
    if (peek(r) == -1) {
        diag_emit(DIAG_ERROR, s, "expected type expression after ':'");
        r->error = true;
        return NULL;
    }
    Form *inner = read_form(r);
    if (!inner) return NULL;
    size_t end = r->pos;
    Span span = span_from_to(r, start_line, start_col, start_off, end);
    return form_type_ann(r->arena, span, inner);
}
```

Place this block between the `::` check and the existing `is_sym_cont` check.

### Step 4 — `elab_type_from_form()` helper

Implement `elab_type_from_form()` in `src/elab.c`:

1. Handle `F_KEYWORD` — existing keyword-to-TypeKind mapping (extracted from current inline dispatch).
2. Handle `F_TYPE_ANN` — unwrap the inner form, recurse.
3. Handle `F_SYM` — look up in type variable environment; fall back to TY_STRUCT with NULL def (opaque type placeholder).
4. Handle `F_LIST` — dispatch on head symbol for `->`, `forall`, `exists`, `vec`, `option`, `either`, `tuple`, `rc`, `ref`, `arr`.

The `(->)` case:
```
(-> T)           — nullary fn returning T
(-> A B)         — unary fn (A → B)
(-> A B C)       — binary fn (A → B → C), i.e. (A, B) → C
```

Multi-argument `->` should produce a `TY_FN` type with the appropriate `arity` and `arg_kinds[]` / `result_kind` fields in `Type.as.fn`. Map each argument sub-form through `elab_type_from_form()` recursively.

### Step 5 — Update annotation sites in `elab.c`

For each site listed in the table above, replace:

```c
if (p->tag == F_KEYWORD) {
    /* inline keyword-to-type mapping */
}
```

with:

```c
if (p->tag == F_KEYWORD || p->tag == F_TYPE_ANN) {
    Type *t = elab_type_from_form(e, p);
    ...
}
```

Defer to `elab_type_from_form()` for the actual mapping. Keep the `F_KEYWORD` branch as-is until all sites are migrated, then consolidate.

### Step 6 — Formatter

Add `F_TYPE_ANN` cases to `fmt_form_flat()` and `fmt_form()` in `src/fmt.c` as described above. Type annotations should always render inline.

### Step 7 — Integration test

1. Move `tests/wip/arrow_tests.tur` back to `tests/arrow_tests.tur`.
2. Run `tur test tests/` — `arrow_tests.tur` should now compile and pass.
3. Run `tur test tests/` on the full suite to check for regressions.

---

## Migration Notes

- Existing `:keyword` syntax (e.g., `:int`, `:bool`) continues to work. The reader only takes the new path when `:` is followed by a non-symbol character (space, `(`, etc.).
- New code should prefer `: type` style. Existing code can be migrated opportunistically.
- The formatter will normalise `:int` → `: int` once `fmt_form()` is taught to emit `F_TYPE_ANN` nodes, but migrating `F_KEYWORD`→`F_TYPE_ANN` in the formatter output is optional for the initial pass.

---

## Files to Change

| File | Change |
|---|---|
| `src/forms.h` | Add `F_TYPE_ANN` to `FormTag` enum |
| `src/forms.c` | Add `form_type_ann()` constructor; handle `F_TYPE_ANN` in any exhaustive switches |
| `src/reader.c` | `read_keyword()` — new `: type-expr` path; add `\|` to `is_sym_start`/`is_sym_cont` |
| `src/elab.c` | Add `elab_type_from_form()`; update annotation sites |
| `src/fmt.c` | Handle `F_TYPE_ANN` in flat and break printers |
| `tests/wip/arrow_tests.tur` | Move back to `tests/arrow_tests.tur` once implemented |

---

## Out of Scope

- Changing existing `:keyword` annotations to `: sym` in stdlib (mechanical migration, separate PR).
- Type inference for polymorphic `->` types (rank-2 inference is a separate effort under `docs/advanced-type-system-feasibility-plan.md`).
- Elaborating `(arr A B)` fully into the HKT typeclass machinery — initial pass only needs the type representation to round-trip without crashing.
