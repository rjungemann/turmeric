# Codegen: cstr literal in `cons` head emitted as `char[]`, not coerced to int64

**Severity:** Medium -- building a cons-list of string literals fails to
compile (`re-string` fixture). A common pattern (list of pattern strings)
does not build.

## Symptom

`tests/fixtures/re-string` -> `tur build failed`:

```
error: incompatible pointer to integer conversion passing 'char[7]'
       to parameter of type 'int64_t' [-Wint-conversion]
 11202 | __t11 = cons(("[0-9]+"), (INT64_C(0)));
error: ... passing 'char[10]' ...
 11203 | __t9 = cons(("[A-Za-z]+"), (__t11));
```

## Repro

`tests/fixtures/re-string/input.tur:12`:

```turmeric
(re/union-patterns-string (cons "[A-Za-z]+" (cons "[0-9]+" 0)))
```

`cons` is lowered to `static int64_t cons(int64_t h, int64_t t)`, but the
string-literal head is emitted as the raw `char[]` array expression
`("[0-9]+")` instead of being cast/coerced to `int64_t`.

## Root cause

At the `cons` call site the cstr literal argument is not run through the
cstr->int64 boxing/coercion that the `int64_t` parameter slot requires. A
cstr bound to a variable and passed to `cons` coerces fine; a **bare string
literal** in head position skips the coercion and lands as `char[]`, which C
rejects for an `int64_t` parameter.

## Fix directions

- In the call-argument lowering, treat a cstr **literal** the same as any
  other cstr rvalue when the target parameter type is `int64_t` (the cons
  head slot): emit the `(int64_t)(intptr_t)"..."` cast (or the standard cstr
  box) rather than the naked array literal.
- Check whether this is specific to `cons` (untyped `int64` head) or general
  to any `int64`-typed parameter receiving a string literal -- likely the
  latter; a fixture with `(some-int64-fn "lit")` would confirm scope.
