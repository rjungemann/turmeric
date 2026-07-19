# Interpreter string natives + pure-Turmeric `range-show` -- Plan

> **Status:** Done (2026-07-13). Phase 0 natives (`str-concat`/`cstr-len`/
> `cstr-nth`) landed in `src/turi/interpreter_natives.c`; the string builders
> (`str-concat`/`int->str`) were factored into the dependency-free leaf
> `stdlib/str-build.tur` and the two formatters rewritten in pure Turmeric; the
> `requires.tur-only` carve is gone. `range-show` passes under both
> `tests/run.sh` (2123/0) and `tests/run-turi.sh`. NB: `int->str` had no
> compiled twin (interpreter-native only), so a compiled `defn` was added; and
> the formatters live in `str-build.tur` rather than loading all of `str.tur`
> because loading `str.tur`'s `Eq [str]` instance into `range-bound`'s graph
> trips a pre-existing `Eq [Bound]` dispatch bug -- see
> `docs/archive/eq-bound-misdispatch-extra-instance.md`.
>
> **Verified 2026-07-19:** still landed and stable. The Phase 0 natives
> (`str-concat`, `cstr-len`, `cstr-nth`, plus `cstr-sub`) are registered in
> `src/turi/interpreter_natives.c` (`turi_env_register_native` at :2969-2972);
> `stdlib/range-bound.tur`'s `range-fmt`/`bound-fmt` (:211, :275) are pure
> Turmeric with zero `` ```c `` blocks; and `tests/fixtures/range-show/` carries
> no `requires.tur-only` marker. All three phases are complete -- nothing
> outstanding. Ready to archive.
> **Last Updated:** 2026-07-19
> **Type:** Interpreter / stdlib
> **Scope:** Un-carve `tests/fixtures/range-show` (`requires.tur-only`) by giving
> the tree-walking interpreter a small, layout-exact set of string-building
> natives, then rewriting `range.tur`/`range-bound.tur`'s two `snprintf`
> formatters in pure Turmeric.
> **Related:** this is the SHARED FOUNDATION for
> [regex-pure-turmeric-port-plan.md](regex-pure-turmeric-port-plan.md) -- the
> `str-concat` / `cstr-nth` / `cstr-len` natives added here are exactly what the
> regex port needs to read input and build output. Do this first; the regex plan
> depends on Phase 0.

---

## Motivation

`range-show` is one of the five `requires.tur-only` interpreter carve-outs. Its
fixture body is pure Turmeric, but `range->str` / `bound->str` bottom out in two
inline-C `snprintf` formatters (`range-fmt`, `bound-fmt` at
`stdlib/range-bound.tur:205` and `:274`), which the tree-walker cannot run.

The carve note calls this "library inline-C, not a language gap." That is true,
but the fix is cheaper than the carve implies, and it removes a foundational
interpreter limitation that also blocks the regex port: **the interpreter has no
runnable string-concatenation or byte-access primitive at all.**

Empirically verified against `./build/tur --interpret`:

- `(int->str 42)` -> `42` (works; native at `interpreter_natives.c:2902`).
- `(str-concat "a" "b")` -> `tur: unknown function or operator 'str-concat'`.
  `str-concat` is inline-C (`stdlib/str.tur:99`) with **no** native override.
- `(cstr-len "hello")` / `(cstr-nth s i)` -> `inline-C not supported in
  interpreter mode` (`stdlib/cstr.tur:26`, `:41`; declined by
  `try_exec_simple_inline_c` because the bodies call `strlen` / index).

So the entire pure-Turmeric string-formatting surface is dead under `--interpret`
(and therefore in the WASM browser REPL, which shares the interpreter). `int->str`
alone works; you cannot glue its output to anything.

---

## Phase 0 -- interpreter string natives (SHARED FOUNDATION)

Add a small set of layout-exact C natives to `src/turi/interpreter_natives.c`,
registered in `turi_env_register_interpreter_natives`
(`interpreter_natives.c:3975`) next to the existing `int->str` / `strcmp` /
`alloc-str` / `cstr-free` cluster (`:2902-2920`). Each mirrors the exact ABI its
compiled `stdlib` counterpart produces (a NUL-terminated `char*` boxed as
`int64`), so a value crossing between interpreted and native code reads
identically. Follow the W1b caveat from the residual plan: match the compiled
layout exactly or it silently miscompiles.

| Native | Mirrors | stdlib source | C behavior |
| --- | --- | --- | --- |
| `str-concat` | `str-concat` | `stdlib/str.tur:99` | `malloc(la+lb+1)`, copy both, NUL-terminate |
| `cstr-len` | `cstr-len` | `stdlib/cstr.tur:26` | `return (int64_t)strlen((char*)a)` |
| `cstr-nth` | `cstr-nth` | `stdlib/cstr.tur:41` | `return (int64_t)(unsigned char)((char*)s)[i]` |

`int->str` already exists, so integer formatting is done. These three are
~6 lines each and follow the established `native_alloc_str` / `native_strcmp_fn`
pattern verbatim (`interpreter_natives.c:490`, `:515`).

**Disposition: fix.** Additive natives only; no codegen touched, no compiled-path
change. The compiled `str-concat` / `cstr-*` are unaffected -- the interpreter
just gains a runnable shim under the same binding name.

**Validation:** `(str-concat "a" (int->str 42))` -> `a42` under `--interpret`;
`(cstr-nth "abc" 1)` -> `98`; `(cstr-len "abc")` -> `3`.

---

## Phase 1 -- pure-Turmeric `range-fmt` / `bound-fmt`

Rewrite the two formatters (`stdlib/range-bound.tur:205`, `:274`) in pure
Turmeric over `str-concat` + `int->str`, deleting the `\`\`\`c` blocks. The logic
is a trivial bracket/paren selection already spelled out in the surrounding
`match` code:

```turmeric
;; kind: 0 = unbounded, 1 = inclusive, 2 = exclusive
(defn bound-fmt [kind : int v : int] : cstr
  (if (= kind 1)
    (str-concat "[" (int->str v))
    (str-concat "(" (int->str v))))

(defn range-fmt [lo-kind : int lo-val : int hi-kind : int hi-val : int] : cstr
  (let [lo (cond (= lo-kind 0) "(-inf"
                 (= lo-kind 1) (str-concat "[" (int->str lo-val))
                 :else         (str-concat "(" (int->str lo-val)))
        hi (cond (= hi-kind 0) "+inf)"
                 (= hi-kind 1) (str-concat (int->str hi-val) "]")
                 :else         (str-concat (int->str hi-val) ")"))]
    (str-concat lo (str-concat ", " hi))))
```

Both now run on both paths. The `expected.stdout` (`[1, 5]`, `[1, 5)`,
`(-inf, 5]`, etc.) is byte-for-byte identical, so no snapshot churn beyond the
fixture itself.

**Disposition: fix.** The `Show [Bound]` instance in `range.tur` (which shares
`bound->str`'s shape, per the note at `range-bound.tur:191`) comes along for free
-- `(show b)` now works under `--interpret` too.

---

## Phase 2 -- drop the carve

1. Delete `tests/fixtures/range-show/requires.tur-only`.
2. `bash tests/run-turi.sh` -- `range-show` now runs and passes under
   `--interpret` (harness +1, one fewer tur-only carve).
3. `bash tests/run.sh` -- compiled path unchanged (formatters produce identical
   output; the only fixture snapshot that could move is a codegen snapshot for
   `range-bound.tur`, regenerate in-PR per CLAUDE.md if it shifts).

---

## Sequencing / definition of done

```
Phase 0 (string natives)  -->  Phase 1 (pure-Turmeric formatters)  -->  Phase 2 (drop carve)
        |
        +--> also unblocks regex-pure-turmeric-port-plan.md Phase 0
```

Done when: `range-show` passes under both `tests/run-turi.sh` and
`tests/run.sh`, its `requires.tur-only` marker is gone, `check_turi_parity.py`
is 0-gap, and `(str-concat ...)` / `(cstr-nth ...)` / `(cstr-len ...)` run under
`--interpret`.

---

## See Also

- [regex-pure-turmeric-port-plan.md](regex-pure-turmeric-port-plan.md) -- consumes
  the Phase 0 natives.
- `docs/archive/history/turi-interpret-flip-residual-plan.md` (Bucket R6) -- where
  `range-show` was carved.
- `src/turi/interpreter_natives.c:2902-2920` (native cluster), `:3975`
  (registration entry point).
