---
title: "S7 -- returning a multi-field `:copy` struct *parameter* directly mis-emits (pointer assigned to a by-value return)"
category: Codegen -- by-value return of a by-pointer struct parameter
severity: Medium. Type-checking passes; the generated C fails to compile.
  A function that returns one of its by-value `:copy` struct parameters directly
  emits `T = const T *` (the param arrives as a pointer, the by-value return
  assigns it as a value). Worked around by rebuilding the result with
  `make-struct` from the param's fields. A 2-field register-class struct returns
  fine; the bug shows up once the struct is passed by pointer (>=3 fields and/or
  float fields -> not register-class).
status: OPEN
reported-by: turmeric-spices Claude (spice-uplift work, branch claude/tender-ramanujan-xm7bgg)
verified-on: turmeric 0.21.0, this tree (post #452 / #456 / #458)
---

# S7 -- returning a multi-field `:copy` struct parameter directly

## One-line summary

A function whose body returns one of its by-value `:copy` struct **parameters**
directly mis-emits in C: the parameter arrives as a pointer (`const T *`,
because a non-register-class struct is passed by pointer), but the by-value
return assigns pointer -> value.

## Reproduction (verified on this tree)

```turmeric
(defstruct B4 :copy [a : float  b : float  c : float  d : float])
(defn pick [x : B4  y : B4] : B4
  (if (> (.a x) 0.0) x y))      ;; returns the param x / y directly
```

`tur check` passes. `tur build` fails in the generated C:

```
/tmp/tur-build/s7_tur.c: error: incompatible types when assigning to type 'B4' from type 'const B4 *'
/tmp/tur-build/s7_tur.c: error: incompatible types when assigning to type 'B4' from type 'const B4 *'
```

(One per returned-parameter arm.)

Control: a 2-field register-class struct returns its parameter fine --

```turmeric
(defstruct P2 :copy [lo : int  hi : int])
(defn pick2 [x : P2  y : P2] : P2
  (if (> (.lo x) 0) x y))      ;; compiles clean, runs correctly
```

-- so the trigger is the struct being passed by pointer, not the act of
returning a parameter.

## Expected

Returning a struct parameter by value copies it out, exactly the same as
returning a freshly `make-struct`'d value (which works). The by-value return
should dereference the incoming pointer when the param is passed by pointer.

## Workaround (in landed spice code)

Rebuild the result with `make-struct` from the param's fields instead of
returning the param:

```turmeric
(make-struct B4 (.a x) (.b x) (.c x) (.d x))
```

This is what plot `bbox-union` does -- each branch rebuilds a fresh `BBox`
rather than returning a parameter (`turmeric-spices`
`spices/plot/src/plot/core.tur:833-852`). The comment at `:835-841` records it:

> Each branch rebuilds a fresh `BBox` with `make-struct` rather than returning a
> parameter directly: a multi-field `:copy` struct is passed by pointer, and
> returning such a parameter by value currently mis-emits in C
> ('incompatible types ... BBox from const BBox *'). Rebuilding from the fields
> sidesteps that.

The rebuild could be replaced with a direct `x` / `y` return once the by-value
return dereferences a by-pointer struct parameter.

## Notes / scope

- A 2-field register-class struct (`[lo : int  hi : int]`) returns its
  parameter fine; the bug appears once the struct is passed by pointer --
  >=3 fields and/or float fields make it non-register-class.
- Smells related to the register-class return work in #453 / #454: the return
  path emits a value-copy assignment without accounting for the pointer
  parameter ABI of a non-register-class struct.
- Fix direction: at a by-value struct return whose returned expression is a
  by-pointer struct parameter (or any lvalue already held as `const T *`),
  emit a dereference (`*p`) rather than assigning the pointer to a `T`.
