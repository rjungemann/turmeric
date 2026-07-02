# HRT poly-call with a non-int-register return emits `-Wint-conversion`

**Summary.** Invoking a rank-2 `forall` poly fn whose result is a
pointer-class type (`cstr`, `ptr<T>`, ...) through the *generic* carrier path
emits the call as `int64_t` with no cast to the concrete result type, so the
surrounding C context (e.g. a `const char *` return slot) triggers
`-Wint-conversion` ("makes pointer from integer without a cast"). Behavior is
correct on LP64 (pointer and `int64_t` are the same width); this is a
cosmetic/portability wart, not a miscompile.

**Severity:** low (warning only; correct result on all current targets).

## Minimal repro

```turmeric
(defn my-show [x : int] : cstr "hi")

(defn use-show [f (forall [a] (-> a cstr))] : cstr
  (f 42))            ;; <- poly-call returning cstr

(defn main [] : int
  (println (use-show my-show))
  0)
```

`tur emit-c` / `tur build` of the above compiles and runs (prints `hi`) but
the C compiler warns:

```
warning: returning 'int64_t' from a function with return type 'const char *'
makes pointer from integer without a cast [-Wint-conversion]
 return f.fn(f.env, (int64_t)(INT64_C(42)));
```

(The constraint vector from slice 2 is *not* required to reproduce -- a bare
`forall [a] (-> a cstr)` shows it. It surfaced while writing the
`forall-constraints` fixtures.)

## Root cause

`src/compiler/emit_expr.c`, the `is_poly_call` invocation block:

- `phase_f_concrete` (emit_expr.c:2658-2664) is only taken when the result
  and every argument kind satisfy `type_kind_is_poly_concrete`, which
  (emit_expr.c:849-853) is `true` **only** for `bool` and the narrow ints
  (`int8/16/32`, `uint8/16/32`). `cstr`, `ptr`, `int64`, and float are all
  excluded.
- With `phase_f_concrete == false`, a unary poly call takes the generic path
  at emit_expr.c:2760-2765, emitting `fn_name.fn(fn_name.env, ...)`. The
  carrier's `.fn` field is typed `int64_t (*)(void*, int64_t)`, so the
  expression's C type is `int64_t`. Only the `phase_f_concrete` branch
  (emit_expr.c:2731-2744) casts `.fn` to a signature with the concrete
  return type `((<ret> (*)(void*, ...))fn.fn)(...)`.
- The `int64_t`-typed result then flows into a `const char *` slot with no
  cast, producing the warning. (The N-ary generic path at 2754-2759 has the
  same shape.)

## Fix directions

Wrap the generic-path result in a cast to the concrete result type when
`e->type` is a pointer/cstr-class kind -- i.e. emit
`((<ret>)(intptr_t)(fn.fn(fn.env, ...)))` for the unary/N-ary generic branches
(emit_expr.c:2754-2765), mirroring the cast the `phase_f_concrete` branch
already applies to `.fn`. Alternatively, broaden `type_kind_is_poly_concrete`
to admit `cstr`/`ptr` so those returns take the already-correct
`phase_f_concrete` cast path -- but that also changes argument handling, so the
narrower result-cast fix is lower risk.
