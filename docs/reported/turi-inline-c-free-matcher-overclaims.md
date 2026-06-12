# inline-C `free` matcher over-claims `*_free(` bodies -> silent miscompile + UAF

> **FIXED (2026-06-12).** `try_exec_simple_inline_c`'s Pattern 1 (`free`) now
> requires a *standalone* `free(` token (`ic_has_standalone_free`) and refuses
> bodies that also return a value or fat-dispatch a closure
> (`!has_return && !has_fptr`). `map-eq-raw?` (and any inline-C body that merely
> calls `tur_hamt_iter_free(`/`tur_hamt_free(`/`xfree(`...) is no longer reduced
> to `free(arg0)`. Found while executing the W1b map-equality work; the fix
> ships alongside the `native_map_eq_raw[_k]` natives that make `map-eq?`
> actually evaluate under `--interpret`.

**Severity:** High (silent miscompile **and** heap-use-after-free). The
interpreter returned a wrong answer *and* corrupted the heap for a whole class
of inline-C functions, not just maps.

## Root cause

`src/turi/eval.c` `try_exec_simple_inline_c` classified an inline-C body by
loose substring match:

```c
bool has_free = strstr(body,"free(");      /* matches tur_hamt_iter_free( too */
...
if (has_free && !has_malloc) {             /* Pattern 1: Free */
    *out = ic_exec_free(args, n_args);     /* free(arg0); return nil */
    return ic_claim("free", fn, out);
}
```

`strstr(body,"free(")` matches any identifier ending in `free(` -- e.g.
`tur_hamt_iter_free(`, `tur_hamt_free(`, `xfree(`. So a function like
`map-eq-raw?` (`stdlib/map.tur:605`), whose body iterates a HAMT and calls
`tur_hamt_iter_free((void*)iter_buf)`, was claimed by the free pattern:
`ic_exec_free` then `free()`d **arg0** (the `Map`'s `{void* hamt}` box) and
returned nil. Two consequences:

1. **Silent miscompile.** `map-eq?` returned nil -> `false` regardless of the
   maps' contents (`(map-eq? a a ...)` on equal maps printed `false`).
2. **Heap-use-after-free.** Because arg0 (the live map box) was freed, a later
   `(map-free a)` double-freed it -- ASan `heap-use-after-free` in `set_hamt`
   (`src/main.c:5995`), freed-by frame `ic_exec_free` (`src/turi/eval.c`).

## Minimal repro (pre-fix)

```turmeric
(defn t [] : bool
  (let [a (:: (map-new) (Map int int))]
    (let [a2 (map-assoc a 1 10)]
      (let [r (map-eq? a2 a2 (fn [x y] (= x y)))]   ; should be true
        (map-free a)
        (map-free a2)                                ; UAF: a2's box already freed
        r))))
(println (t))
```

```sh
ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret repro.tur
#   pre-fix: AddressSanitizer: heap-use-after-free in set_hamt
#   post-fix: true (and the map-eq fixture prints true/true/false)
```

## Fix

`src/turi/eval.c`:

- `ic_has_standalone_free(body)` -- scans for a `free(` token whose preceding
  character is not `[A-Za-z0-9_]`, so `*_free(` no longer matches.
- Pattern 1 now also requires `!has_return && !has_fptr`: a genuine destructor
  is `free(p);` with no value return and no `(*)(` fat-dispatch. `map-eq-raw?`
  has both, so even a hypothetical standalone `free(` inside it would be
  excluded.

With the matcher tightened, `map-eq-raw?`/`map-eq-raw-k?` fall through to their
registered natives (`native_map_eq_raw[_k]`, `src/main.c`), which iterate the
HAMT and invoke the value comparator (a turi closure under `--interpret`) via
`turi_call` -- the same closure-caller pattern as `native_result_eq`.

## Validation

- `map-eq`, `typed/map-eq` pass under `--interpret` and join the `run-turi.sh`
  allowlist (harness 985 -> 987, 0 failed).
- Full compiled suite unchanged; `check_turi_parity.py` clean (112/115, 0 gaps).
- The inline-C `free`-pattern fixtures (plain destructors) still pass.

## Follow-up (recursive container values) -- RESOLVED 2026-06-12

`map-of-tvec-eq` / `set-of-tvec-eq` (maps/sets whose *values*/elements are
themselves containers) were initially still mis-rendering, because the recursive
comparator bottoms out in `vec-eq?` / `set-eq-cmp?`, whose inline-C bodies
fat-dispatch the comparator through a C function pointer the simple executor
cannot run. Closed by registering `native_vec_eq` (`vec-eq?`) and
`native_set_eq_cmp` (`set-eq-cmp?`) -- same `turi_call`-the-comparator pattern as
`native_map_eq_raw`. Unblocked `map-of-tvec-eq`, `set-of-tvec-eq`,
`vec-of-tvec-eq`, `vec-of-tvec-eq-manual`, `typed/vec-basic` (harness 987 ->
992). Tracked in
[turi-map-set-hamt-interpreter-gap.md](turi-map-set-hamt-interpreter-gap.md).
`eq-carrier-capturing-comparator` remains a gap -- it additionally needs the
`mutmap-*` collection natives (a separate surface).
