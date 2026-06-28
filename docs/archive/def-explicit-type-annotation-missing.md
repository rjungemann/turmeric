# Missing Type Annotation and Type Parameter Support on 'def'

**Severity:** low -- usability and ergonomics enhancement. Has a robust, existing workaround via explicit `(::)` type ascription.

## Summary

Users frequently expect top-level variable and constant definitions (`def`) to support explicit type annotations or type parameters directly, mirroring standard bindings like `defn` or local `let` variables. For example:

```turmeric
(def my-value : cstr "Hi!")
```
```sweet-exp
def my-value :cstr "Hi!"
```

Currently, writing this results in a hard compilation error:
> `def takes (def [^persistent] [^deprecated ["msg"]] name init)`

---

## Root Cause

The issue lies in `elab_def` within `src/compiler/elab_fns.c`. The parser and elaborator enforce a strict syntax check on the length of the `def` list form:

```c
if (name_idx + 2 != call->as.list.len) {
    diag_emit(DIAG_ERROR, call->span,
              "def takes (def [^persistent] [^deprecated [\"msg\"]] name init)");
    return NULL;
}
```

This strict shape validation completely blocks any additional items in the form, such as a type annotation `: Type` or type parameter brackets `[T]`.

---

## Current Workaround

If a specific type must be enforced rather than relying on automatic inference of the initializer type, you must ascribe the type onto the initializer expression itself using the `(::)` ascription operator:

```turmeric
(def my-value (:: "Hi!" cstr))
```
```sweet-exp
def my-value ::("Hi!" cstr)
```

While functional, this syntax is less intuitive for users coming from other typed languages or other parts of Turmeric where `: Type` annotations are placed directly adjacent to the binder name.

---

## Suggested Implementation Plan

To align `def` with `defn` and other binding forms, the elaborator can be updated as follows:

1. **Update `elab_def` to detect `: Type` signatures**:
   Add an optional check after parsing the name. If the next form is a type annotation (either starting with `:` or having type annotation metadata), extract it as the declared type of the definition.

2. **Desugar or Constrain the Initializer**:
   - Parse `(def name : Type init)`.
   - Syntactically lower this to a type ascription on the initializer expression (`(:: init Type)`), OR
   - Directly unify the type of the elaborated `init` expression with the declared `Type` before creating the final `Binding`.

3. **Align Diagnostics**:
   Ensure `diag_emit` error messages are updated to reflect the new, richer syntax:
   `def takes (def [^persistent] [^deprecated ["msg"]] name [: type] init)`
