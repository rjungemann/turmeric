# Set Literal Syntax — `#s(...)` Implementation Plan

> **Status:** Planned  
> **Prerequisites:** None — can land at any point after Phase 19  
> **Related:** [turmeric-plan.md §12.5.2](turmeric-plan.md#1252-conflicts-with-turmericss-existing-reader-and-how-to-resolve-them) (notation conflict table)

---

## Motivation

Turmeric needs a set literal form — an unordered, deduplicated collection — analogous to vectors (`[...]`) and maps (`#{...}`). This plan documents the notation choice and the implementation work required to ship it.

---

## Notation Decision

Several Clojure- and Racket-inspired notations were considered. All obvious candidates were already taken:

| Candidate | Problem |
|---|---|
| `#{}` | **Taken** — map literals (`#{:x 1 :y 2}`), stable since Phase R5 |
| `#[]` | **Taken** — attribute syntax (`#[no-unwind]`), stable since Phase R5 |
| `{}` | **Taken** — reserved for SRFI-105 curly-infix (Phase S1); rejected at read time in plain `#lang turmeric` with a hint message |
| `[]` | **Taken** — vector literals; also used as neoteric bracket in `f[x]` (Phase S2) |

**Decision: `#s(...)`** — reader dispatch via `#s` prefix, using parentheses as the delimiter.

Rationale:
- `#s` is mnemonic for "set."
- Parentheses avoid any conflict with `#[` (attributes) and `#{` (maps).
- The `#<char>` dispatch pattern (`#s`, `#t`/`#f` for booleans, `#lang`, `#|` for block comments) is already familiar to Lisp users; adding a new dispatch character is low cognitive overhead.
- `#s(...)` is already used in Common Lisp for structure literals; Turmeric's use is analogous enough that readers familiar with CL will find it intuitive.
- No sweet-expression tier (S1–S3) touches `#s`; there is zero conflict with the SRFI-110 plan.

**Alternatives that were not chosen:**
- `##(...)` — technically free, but `##` reads as "double hash," which has no precedent in the Lisp family and would confuse readers.
- `set(1 2 3)` — neoteric function call, not a literal; would require the reader to know about `set` as a special identifier, breaking the clean reader/elaborator separation.
- `#,(...)` — taken by reader-extension conventions in some Scheme implementations; confusing in a quasiquote context where `,` is meaningful.

---

## What Changes

### 1. `src/forms.h` — New `F_SET` tag

Add `F_SET` to the `FormTag` enum, with the same payload structure as `F_LIST`, `F_VEC`, and `F_MAP` (a `FormList` of elements).

```c
F_SET,        /* #s(a b c) — same payload as F_LIST */
```

Add the constructor declaration:

```c
Form *form_set(Arena *a, Span span, Form **items, uint32_t len);
```

### 2. `src/forms.c` — Constructor and printing

**Constructor** — mirrors `form_map`:

```c
Form *form_set(Arena *a, Span span, Form **items, uint32_t len) {
    return form_seq(a, F_SET, span, items, len);
}
```

**`form_tag_name`** — add case:

```c
case F_SET: return "set";
```

**`form_print`** — add case that emits `#s(...)`:

```c
case F_SET:
    buf_push_str(b, "#s(");
    for (uint32_t i = 0; i < f->as.list.len; i++) {
        if (i) buf_push_char(b, ' ');
        form_print(b, f->as.list.items[i]);
    }
    buf_push_char(b, ')');
    break;
```

### 3. `src/reader.c` — Dispatch on `#s(`

In `read_form`, add a check before the existing `#` dispatch cases. `#s(` is a three-character lookahead: `peek(r) == '#'`, `peek2(r) == 's'`, `peek3(r) == '('`.

```c
if (c == '#' && peek2(r) == 's' && peek3(r) == '(') {
    return read_set(r);
}
```

**`read_set` function:**

```c
static Form *read_set(Reader *r) {
    advance(r); /* consume '#' */
    advance(r); /* consume 's' */
    return read_seq(r, '(', ')', F_SET, "unterminated set (missing ')')");
}
```

`read_seq` already handles the general case; `F_SET` just needs a different tag.

> **Note on deduplication:** The reader does *not* deduplicate at read time. Deduplication happens at elaboration/runtime, consistent with how map key collisions are handled (the elaborator or runtime is responsible for semantics; the reader is a dumb token lifter). An `#s(1 1 2)` at the source level is a valid parse; the elaborator or runtime decides whether duplicate elements are an error, a warning, or silently deduplicated.

### 4. `src/elab.c` — Elaborator support

The elaborator has several places where it switches on `F_MAP` alongside `F_VEC`. Each needs a corresponding `F_SET` case. Audit the following patterns and add `case F_SET:` alongside `case F_MAP:` where it makes sense:

- **Quasiquote expansion** (the `case F_VEC: case F_MAP:` block around line 1247): sets inside quasiquote should splice-expand their elements, identical to maps and vectors.
- **`ct_value` / compile-time value representation** (around line 1248, 1639, 1741, 1885): sets are values; treat them analogously to maps.
- **`empty?` predicate** (line 1342): `#s()` should count as empty.
- Any other switch on `FormTag` that handles `F_MAP` — grep `F_MAP` in `elab.c` and decide case by case.

Where a pass currently handles `F_MAP` by building a `tur_map_t *`, the `F_SET` branch will build a `tur_set_t *` (see §Runtime below).

### 5. Runtime representation

Sets need a C-level type. Two approaches:

**Option A — Sorted array** (simple, good for small sets):  
`tur_set_t` is a `tur_val_t[]` sorted by a canonical ordering. `set-member?` is binary search. Construction deduplicates by sorting and dropping adjacent equal elements.

**Option B — Hash set** (consistent with map):  
`tur_set_t` mirrors `tur_map_t` but stores only keys. Membership test is O(1).

**Recommendation:** Ship Option A for v1 (small, self-contained, no new hash machinery). The type is opaque behind the `tur_set_t` typedef; switching to Option B later is a runtime-internal change with no surface-syntax impact.

### 6. Stdlib operations

Once the type exists, the minimal useful set of operations:

| Function | Signature | Notes |
|---|---|---|
| `set-member?` | `(set-member? s x)` → `bool` | Test membership |
| `set-add` | `(set-add s x)` → `set` | Functional add; returns new set |
| `set-remove` | `(set-remove s x)` → `set` | Functional remove |
| `set-union` | `(set-union s1 s2)` → `set` | All elements from either |
| `set-intersection` | `(set-intersection s1 s2)` → `set` | Elements in both |
| `set-difference` | `(set-difference s1 s2)` → `set` | Elements in `s1` not in `s2` |
| `set->list` | `(set->list s)` → `list` | Convert to list (unspecified order) |
| `list->set` | `(list->set xs)` → `set` | Build set from list, deduplicating |
| `set-count` | `(set-count s)` → `int` | Number of elements |
| `set-empty?` | `(set-empty? s)` → `bool` | Sugar for `(= 0 (set-count s))` |

---

## Implementation Checklist

### Phase X1 — Reader + AST (pure syntax, no runtime)

- [ ] Add `F_SET` to `FormTag` in `src/forms.h`
- [ ] Add `form_set()` declaration in `src/forms.h`
- [ ] Implement `form_set()` in `src/forms.c`
- [ ] Add `case F_SET: return "set";` in `form_tag_name()` in `src/forms.c`
- [ ] Add `F_SET` print case (`#s(...)`) in `form_print()` in `src/forms.c`
- [ ] Add `read_set()` function in `src/reader.c`
- [ ] Add `#s(` dispatch in `read_form()` in `src/reader.c`
- [ ] Fixture: `set-read-basic.tur` — `#s(1 2 3)` parses and round-trips via `form_print`
- [ ] Fixture: `set-read-empty.tur` — `#s()` parses as empty set
- [ ] Fixture: `set-read-nested.tur` — `#s(#s(1) #s(2))` nested sets parse correctly
- [ ] Fixture: `set-read-keywords.tur` — `#s(:a :b :c)` keyword elements
- [ ] Negative fixture: `set-unterminated.tur` — `#s(1 2` emits "unterminated set (missing ')')"

### Phase X2 — Elaborator integration

- [ ] Audit all `case F_MAP:` sites in `src/elab.c`; add `case F_SET:` where appropriate
- [ ] Quasiquote: `\`#s(~x ~y)` splices correctly
- [ ] `empty?`: `#s()` returns true; `#s(1)` returns false
- [ ] Compile-time value representation handles `F_SET`
- [ ] Fixture: `set-quasiquote.tur` — set literals inside quasiquote expand correctly
- [ ] Fixture: `set-empty-predicate.tur` — `(empty? #s())` returns true

### Phase X3 — Runtime type + stdlib

- [ ] Define `tur_set_t` in `src/runtime.h` (sorted-array representation, v1)
- [ ] Implement `tur_set_new`, `tur_set_from_array` (deduplicates) in `src/runtime.c`
- [ ] Codegen: `F_SET` literal lowers to `tur_set_from_array(...)` call
- [ ] Implement stdlib functions listed above in `src/stdlib.c` (or equivalent)
- [ ] Fixture: `set-basic.tur` — construct, member?, count
- [ ] Fixture: `set-operations.tur` — union, intersection, difference
- [ ] Fixture: `set-from-list.tur` — `list->set` deduplicates
- [ ] Negative fixture: `set-duplicate-elements.tur` — `#s(1 1 2)` deduplicates silently (or warn — decide before shipping)

---

## Open Questions

1. **Duplicate elements at read time:** Silently deduplicate, warn, or error? Recommendation: silently deduplicate (consistent with how most Lisps and Clojure handle `#{1 1}` — they error, but our reader-vs-elaborator split makes silent dedup at elaboration time cleaner). Decide before Phase X3 ships.

2. **Ordering of `set->list`:** Should the iteration order of a set be deterministic (sorted)? Sorted order is free with Option A and makes tests reproducible. Unspecified order is more honest if we ever switch to a hash set. Recommendation: document as unspecified but implement as sorted in v1.

3. **Equality semantics:** Two sets are equal if they contain the same elements (structural equality). Ensure `equal?` / the `Eq` typeclass handles `tur_set_t` consistently with `tur_map_t`.

4. **Sweet-expression interaction:** `#s(` inside a sweet-indented block should behave like `(` — indentation processing is suppressed inside the parens. Verify this works when Phase S3 ships; no changes expected since the outer delimiters are `(` / `)`.
