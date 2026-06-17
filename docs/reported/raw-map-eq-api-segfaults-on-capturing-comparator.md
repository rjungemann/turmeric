# Raw `map-*-eq` API segfaults when handed a capturing closure comparator (compiled path)

**Summary:** The raw content-keyed map API (`map-assoc-eq` / `map-get-eq` /
`map-has-eq?` / `map-dissoc-eq`, stdlib/map.tur) declares its `keyeq` parameter
*untyped* and its inline-C body casts it straight to a bare C function pointer
(`(void *)(intptr_t)keyeq`). Passing a **capturing** closure there compiles
without complaint and then **segfaults at runtime** on the compiled path -- the
fat-closure box pointer is called as if it were `bool(*)(int64_t,int64_t)`. The
interpreter handles the same source correctly (TI10 Tier B routes a
`TURI_CLOSURE` comparator through the `tur_hamt_*_eq_ctx` trampoline), so this is
also a `tur` vs `turi` divergence.

**Severity:** Medium. Hard crash (SIGSEGV), not a silent miscompile, and only on
*misuse* of the raw API -- but the type checker gives no help: an untyped `keyeq`
accepts a capturing closure that the inline-C contract cannot honor. The
type-erasing untyped parameter is exactly the kind of hole CLAUDE.md's "No Lazy
`:int` Stand-Ins" rule warns about, one level removed (here it is an *untyped*
param rather than `:int`, but the effect is the same: the compiler cannot reject
a value the callee cannot use).

## Minimal repro

```turmeric
(defn main [] : int
  (let [k   42
        cmp (fn [a : int b : int] : bool (= (+ a k) (+ b k)))   ;; captures k
        m   (map-assoc-eq (map-assoc-eq (:: (map-new) (Map int int)) 0 1 100 cmp)
                          0 1 200 cmp)]
    (println (map-get-eq m 0 1 cmp)))
  0)
```

| Path | Result |
| --- | --- |
| `tur run` (compiled) | **Segmentation fault** (exit 139) |
| `tur --interpret`    | `200` (correct) |

A *captureless* lambda or a top-level `defn` works on both paths, because the
elaborator lowers it to a plain C function pointer -- which is what the inline-C
expects.

## Root cause

`stdlib/map.tur`:

```turmeric
(defn map-assoc-eq [K V]
  [m (Map K V) h :int key :K val :V keyeq]      ; <-- keyeq is UNTYPED
  : (Map K V)
  ```c
  ...
  void *new_hamt = tur_hamt_set_eq(map->hamt, (uint64_t)h, (void *)(intptr_t)key,
                                   (void *)(intptr_t)val,
                                   (void *)(intptr_t)keyeq);   ; <-- cast to C fn ptr
  ...
  ```)
```

`tur_hamt_set_eq` (src/runtime/hamt.c) takes
`tur_hamt_keyeq_fn = bool(*)(int64_t,int64_t)` and, on a hash collision, calls
`keyeq(a, b)`. When `keyeq` is a capturing closure, the value passed is a
pointer to the closure's fat box, not code; calling it jumps into data -> SIGSEGV.
The contract the inline-C assumes ("`keyeq` is an int64 carrying a C function
pointer address" -- what a `MapKey` `mk-cmp` instance returns) is undocumented at
the type level, so nothing rejects a closure.

This is the compiled-path mirror of what TI10 Tier B fixed for the interpreter:
the interpreter's `native_map_assoc_eq` (src/main.c) detects
`a[4].tag == TURI_CLOSURE` and routes through `tur_hamt_set_eq_ctx` +
`map_turi_eq_tramp`, so a closure works there.

## Expected behavior

One of:

1. **Reject at elaboration.** Give `keyeq` a function-pointer type the checker
   can enforce (a non-`^fat`, captureless `(fn [K K] bool)`), so a capturing
   closure is a compile error pointing the user at either a top-level `defn` /
   captureless lambda, or the typeclass `Map[K V]` surface.
2. **Support it.** Declare `keyeq` `^fat` and fat-dispatch it in the inline-C +
   thread a ctx through `tur_hamt_*_eq` on the compiled path too (the `_eq_ctx`
   family already exists in the runtime for the interpreter). This is the larger
   fix and carries fixture-snapshot churn for the new ABI.

Direction (1) is the smaller, safer change and matches the API's actual intended
use (the typed `map-assoc`/`map-get`/... macros always feed `mk-cmp`'s C address).

## Validation of a fix

- The minimal repro above should either compile-error (direction 1) or print
  `200` on both paths (direction 2).
- `tests/fixtures/tib-map-reentrant-comparator/` (added alongside this report)
  exercises the *supported* shape -- top-level captureless comparators reading a
  global map -- and passes on both paths; it should stay green.
- `bash tests/run.sh` and `bash tests/run-turi.sh` stay green.

## Notes

Found while adding interpreter-reentrancy coverage for TI10 Tier B
(`docs/upcoming/turi-parity-post-v1-plan.md`). Tier B itself (the interpreter
side) is complete and correct; this report is strictly about the *compiled*
raw-API footgun and is Track A / map.tur surface, not interpreter work.
