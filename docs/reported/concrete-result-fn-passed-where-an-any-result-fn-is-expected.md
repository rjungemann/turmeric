# A concrete-result function passed where an `any`-result function is expected gets no adaptor

**Severity: medium-high.** It is undefined behaviour reached with no
diagnostic: a mismatched function-pointer call. It is measured as a panic on
x86-64 Linux, and it is not guaranteed to be one. Typed Turmeric, no
typeclass needed; `--interpret` answers. Filed 2026-09-28, found while
reducing
[erased-instance-body-tags-a-type-variable-widened-to-any](erased-instance-body-tags-a-type-variable-widened-to-any.md).

The checker accepts a `(fn [float float] float)` argument for a `(fn [float
float] any)` parameter (a result widened into `any`), and nothing converts it.
The callee invokes the argument through a thunk that returns a 16-byte
`tur_tagged_t`, while the function actually returns a `double`.

## Repro

```turmeric
(defstruct Pt [x : float y : float])
(defn ap2 [p : Pt g : (fn [float float] any)] : any
  (g (.x p) (.y p)))
(defn main [] : int
  (println (cast (ap2 (Pt 1.5 2.25) (fn [a : float b : float] : float (+ a b))) float))
  0)
```

Measured 2026-09-28 with `./build/tur` (v0.56.2, x86-64 Linux, gcc 13):

- `tur --interpret`: `3.75` (expected).
- `tur run`: `panic at ...: cast: any holds unknown, not float`, exit 134.

The callee's call, built from the parameter's declared type:

```c
tur_tagged_t __ps_172 = ((*( tur_thunk_tur_tagged_t_double_double_t *)((void *)(intptr_t)(g)))((void *)(intptr_t)(g), (double)(p).x, (double)(p).y));
```

On SysV x86-64 the caller reads the `tur_tagged_t` from `rax:rdx`, while the
callee left its `double` in `xmm0`. The tag is whatever `rax` held, which here
was not a valid tag, so `cast` caught it. A stale `rax` that happens to hold a
valid tag would give a silent wrong answer. On Win64 a 16-byte struct is
returned through a hidden pointer, and the open
[static-instance-spec-calls-any-lambda-as-concrete-result](static-instance-spec-calls-any-lambda-as-concrete-result.md)
saw that mismatch crash with no output there. This one was not measured on
Windows.

The generic spelling reaches the same place, because the type variable is
fixed to `any` by the declared result rather than taken from the argument:

```turmeric
(defn ap1 [B] [g : (fn [float] B)] : any
  (g 1.5))
(defn main [] : int
  (println (cast (ap1 (fn [a : float] : float (+ a 2.25))) float))
  0)
```

`--interpret` prints `3.75`; `tur run` panics the same way. The generic body
itself calls `g` through `tur_thunk_tur_tagged_t_double_t`, and the only spec
is `ap1__spec__tur_tagged_t_int64_t`. So `(g 1.5) : B` was unified with the
`: any` result at the definition, and `B` became `any` there. With the result
declared `: B` instead, the same program prints `3.75` on both back ends.

## Root cause

The argument seam marshals functions in one direction only. When the argument
is an `any`-returning (or `any`-holding) function and the parameter is typed,
`saffron_seam_fn_adaptor` (`src/compiler/elab_call.c:761`) wraps it in a
source-level adaptor:

```
(let [__sfn <arg>] (fn [__sa0 : A ...] : R (cast (__sfn __sa0 ...) R)))
```

(fixture `saffron-seam-into-typed-fn-param`). The opposite direction, a typed
function flowing into a parameter whose parameter or result types are `any`,
is accepted by the checker and passed through as-is. `fn_type_is_all_any`
(just above, ~753) declines the all-`any` target as "nothing to marshal",
which is true only when the argument is already all-`any`.

For the generic spelling, the checker lets the body's `B`-typed result meet
the declared `any` by binding `B := any`, not by widening a `B` to `any` at
the return. The widen form is what `(defn wrap [A] [x : A] : any x)` gets, and
it works.

## Fix directions

1. Add the mirror adaptor at the argument seam. When the argument's fn type
   differs from the parameter's only by `any` in parameter or result
   positions, wrap it:
   `(let [__sfn <arg>] (fn [__sa0 : P0 ...] : any (:: (__sfn __sa0 ...) any)))`
   It unboxes `any` parameters to the argument's types with `cast`, and
   widens the result. It is the same construction as
   `saffron_seam_fn_adaptor`, run in the other direction.
2. Keep a rigid type variable rigid. `(g 1.5) : B` returned under `: any`
   should elaborate as a widen of `B`, with `B` then instantiated from the
   argument at the call, as for `wrap`. Alternatively refuse the program with a
   diagnostic rather than silently fixing `B := any`.
3. Add fixtures for both repros on both back ends.
