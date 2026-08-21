# catch-unwind over a thunk returning a by-value aggregate is miscompiled (segfault)

**Severity: medium-high** (memory unsafety, silent). `(catch-unwind (fn [] : S ...))`
where `S` is a struct/ADT with concrete codegen layout reads an arbitrary
`int64_t` as a pointer to `S`. Found 2026-08-20 while attempting
`json-str-result-and-file-readers-missing`, which this blocks.

**It is the SUCCESS path that breaks, not the panic path.** No panic need be
involved, which is what makes it easy to miss.

## Repro

```turmeric
(defstruct Q [x : int y : int])
(defn f [] : (Result Q int) (catch-unwind (fn [] : Q (make-struct Q 3 4))))
(defn main [] : int (let [r (f)] (println (.x (ok-val r)))) 0)
```

```
$ tur run repro.tur
Segmentation fault
```

`:copy` and non-`:copy` structs behave identically.

## Boundary (each probe run, not assumed)

| Probe | Result |
|---|---|
| `catch-unwind` thunk returning `: int` | **works** (`ok`, `7`) |
| `(Result Q int)` built directly with `(ok (make-struct Q 3 4))`, no catch-unwind | **works** (`ok`, `3`) |
| `catch-unwind` -> struct, but only `(ok? r)`, never `ok-val` | **works** (prints `ok`) |
| `catch-unwind` -> struct, then `(ok-val r)` | **SEGFAULT** |

So `(Result S E)` is sound on its own, and the box's tag survives; only the
payload is wrong. The `: int` case working is why `stdlib/panic.tur`'s own
example (`(catch-unwind (fn [] :int (risky)))`) never showed it.

## Root cause

Two ends of the same call disagree about the thunk's return type.

`src/compiler/emit_expr.c:4018` (`EX_CATCH_UNWIND`) lowers unconditionally to
`tur_catch_unwind_box((int64_t)(intptr_t)thunk)` with no consideration of the
thunk's return type. That helper calls the thunk through `TUR_APPLY0`:

```c
static int64_t tur_catch_unwind_box(int64_t thunk) {
    ...
    int64_t __v = TUR_APPLY0(thunk);     /* TUR_APPLY0_T(int64_t, f) */
    ...
    return tur_box_ok(__v);
}
```

and `TUR_APPLY0_T` casts the function pointer to `int64_t (*)(void *)`. But the
thunk lambda is emitted with its declared Turmeric return type:

```c
static tur_adt_Q __fn_1331();          /* returns the struct BY VALUE, no params */
```

Calling that through an `int64_t (*)(void *)` pointer is UB: the aggregate
comes back in a register pair or via a hidden sret pointer, and whatever
lands in the int64 slot is boxed as `ok_val`.

The consumer then treats that value as a **pointer**:

```c
tur_adt_Result__Q__int __t170 = (tur_adt_Result__Q__int){
    .is_ok  = __t169->is_ok,
    .ok_val = (tur_adt_Q *)(intptr_t)(__t169->ok_val),   /* <-- garbage as ptr */
    .err_val = __t169->err_val };
```

so the intended contract is clearly "thunk hands back a pointer to the
aggregate"; the bug is that nothing arranges for that to be true. `.x` then
dereferences it.

Note the failure is a segfault only because the garbage happens not to be a
mappable address -- it is an arbitrary integer reinterpreted as a pointer, so
corruption is equally available.

## Fix direction

Make the two ends agree for aggregate returns. Either:

1. Box in the thunk: when a catch-unwind/catch-panic-of thunk's return type is
   an aggregate with concrete codegen layout, emit it returning a heap-boxed
   pointer, which is what the consumer at the `tur_adt_Result__*` construction
   already assumes; or
2. Use the sret path already in the preamble: `tur_catch_unwind` +
   `tur_thunk_fn` (`void (*)(void *env, tur_result *out)`) writes through an
   out-parameter and does not go through the `int64_t` apply macro at all.

(1) is the smaller change and matches the existing consumer. Either way
`EX_CATCH_PANIC_OF` (`emit_expr.c`, immediately below) needs the same
treatment -- `tur_catch_panic_of_box` has the identical `TUR_APPLY0` shape.

Worth a fixture in both shapes (ok path and panic path, struct payload), since
the ok path is the one that breaks and an ADT-payload test would otherwise
only ever exercise the err slot.

## Blocks

- `docs/reported/json-str-result-and-file-readers-missing.md` -- a
  Result-returning `#json-str?<T>` decodes into a struct by definition, so the
  natural `catch-unwind`-based expansion hits exactly this.
