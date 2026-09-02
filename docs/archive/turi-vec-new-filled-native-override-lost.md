# `vec-new-filled` lost its native override under `--interpret`, and `bit-shr` was silently wrong everywhere -- ALL FIXED

**Severity: medium (vec-new-filled), high (bit-shr).** Three related-but-distinct
defects found 2026-09-01 while getting `performance-comparison/`'s `turi`
column to actually run instead of silently failing type-checking, all fixed
the same day. `bit-shr` turned out not to be interpreter-only: it was wrong
in the **compiled and JIT paths too**, silently returning an arithmetic
(sign-extending) shift instead of the documented logical one for any
negative operand.

## 1. `vec-new-filled`'s native-override registration timing -- FIXED

`native_vec_new_filled` (`src/turi/collections_native.c`) is registered by
`turi_register_collection_natives`, which is called **exactly once**, at
env-creation time (`src/turi/env.c`), before any preload runs. Every other
benchmark-only native (`cons`, `head`, `tail`, `hamt-new`, the `io-*` family,
...) is registered by `wk_register_stdlib_natives` /
`turi_env_register_interpreter_natives`, which is deliberately called a
**second time**, in `main.c` (and `tur repl`, and macro-env), strictly after
all `turi_env_preload_*` calls -- specifically so a native shim wins over any
stub or module body declared during preload.

`vec-new-filled` had no such second registration. Its type stub (originally
`(defn vec-new-filled [n :int v :int] :int 0)` in
`turi_env_preload_native_stubs`) is itself wrong per CLAUDE.md's "No Lazy
`:int` Stand-Ins" rule -- `native_vec_new_filled` builds the exact
`{data,len,cap}` layout `defstruct Vec` expects, so the return type must be
`(Vec A)`. But `(Vec A)` cannot be declared at that point either: `Vec` isn't
bound yet (that stub block runs before `vec.tur` preloads), so the annotation
hits `TUR-E0012 kind mismatch`.

**Fix:** moved the stub to the end of `turi_env_preload_collections` (after
`vec.tur` loads, so `(Vec A)` resolves) **and** added a
`turi_register_collection_natives(env)` re-call right after each existing
late `turi_env_register_interpreter_natives(env)` call (`main.c`, `repl.c`,
`macro_env.c`) -- mirroring the pattern the other natives already rely on.
Verified: `(let [s (vec-new-filled 5 0)] (println (vec-len s)))` went from
printing `0` (the stub's own empty-vec body ran) to `5` (the native ran).
This unblocked `primes`, `alloc_churn`, `matrix_multiply`, `string_concat`,
and `text_search` under `--interpret`.

Six benchmark source files (`primes.tur`, `sort.tur`, `alloc_churn.tur`,
`matrix_multiply.tur`, `string_concat.tur`, `text_search.tur` under
`performance-comparison/benchmarks/*/turi/`) also had their own `:int`-typed
vec parameters retyped to `(Vec int)` -- the same class of bug, in benchmark
source rather than the compiler, exposed once `vec-new-filled`'s own type
stopped being `:int`.

## 2. `bit-shr` performed an arithmetic (signed) shift, not the documented logical one -- FIXED, was NOT interpreter-only

Initial read: `native_bit_shr` (`src/turi/interpreter_natives.c`) is
implemented correctly and registered inside the early+late double-registration
pattern above, and `stdlib/bits.tur` (the real `defn`) is never loaded under
`--interpret` at all -- so there was no competing stub or registration-timing
bug to find here, unlike (1). And yet `(bit-shr -1 1)` returned `-1` under
`--interpret` instead of the documented `9223372036854775807`.

**Root cause:** `bit-shr` is not a plain function call at all -- it is a
**compiler builtin** (`src/compiler/builtins.c`: `{ "bit-shr", ..., BS_BIN_INFIX,
">>" }`), lowered directly to a bare C `(a) >> (b)` on `int64_t` (signed)
operands. `native_bit_shr` is never invoked; `native_bit_shr` correctly casts
to `uint64_t` first, but that native is now dead code as far as `bit-shr`
call sites are concerned (`bit-shr` never resolves to a plain function call).
`>>` on a signed negative value is an arithmetic (sign-extending) shift on
every real C compiler -- exactly backwards from the "logical (unsigned) right
shift... 0-fills high bits" contract `stdlib/docstrings.tur` and
`stdlib/bits.tur`'s own doc comment promise.

**This bug was not interpreter-only.** `BS_BIN_INFIX`'s generic `"(a) %s (b)"`
splice is shared by the compiled-C emitter (`emit_core.c`), the CPS-IR emitter
(`emit_cps_ir.c`, which the JIT also compiles), and the interpreter's own copy
of the same dispatch (`eval.c`) -- all three independently reproduced the bug:

```
$ tur build <(echo '(println (bit-shr -1 1))') -o /tmp/t && /tmp/t   # -1 (wrong)
$ tur jit   <(echo '(println (bit-shr -1 1))') --                    # -1 (wrong)
$ tur --interpret <(echo '(println (bit-shr -1 1))')                 # -1 (wrong)
```

`>>` is used by no other builtin (`grep '">>"' src/compiler/builtins.c`), so
it was safe to special-case it in all three sites without touching any other
`BS_BIN_INFIX` operator (`==`, `<`, `mod`, ...):

- `emit_core.c` (`BS_BIN_INFIX` case): emits
  `(int64_t)((uint64_t)(a) >> (uint64_t)(b))` when `spec->c_op == ">>"`.
- `emit_cps_ir.c` (`prim_expr`'s `BS_BIN_INFIX` case): same.
- `eval.c` (`BS_BIN_INFIX` case, `op == ">>"` branch): same cast, computed
  directly on the `TuriValue`s.

Verified fixed across all three execution engines (compiled, `tur jit`, and
`--interpret`), plus the same case reached through a function parameter
(ruling out constant-folding as either the bug or the fix).
`data_structures/turi/sort.tur`'s min/max output went from a negative number
to a plausible positive magnitude.

## 3. `int->float` had no stub at all under `--interpret` -- FIXED

Unlike `bit-shr`, `int->float` has **no entry in `src/compiler/builtins.c`**
-- it is not a compiler builtin. In the compiled path this doesn't matter:
every compiled benchmark that uses it (`numerical/turmeric/monte_carlo_pi.tur`)
defines its own local `int->float` inline-C helper. But the `turi`-only
`numerical/turi/monte_carlo_pi.tur` calls a bare `(int->float inside)`
assuming global availability, and nothing had ever declared its signature to
the elaborator: `TUR-W0040 unknown name`, then a hard `TUR-E0042 mixed-width
numeric arithmetic` the moment the (untyped) result reached a float context.
`native_int_to_float` (`interpreter_natives.c`) exists, correctly implemented
and correctly registered (in `wk_register_stdlib_natives`, covered by the
early+late pattern) -- it was simply never reachable because the elaborator
never had a declared signature to resolve the bare name against.
`preload.c`'s own comment claiming `int->float` "resolves at elaboration"
alongside `bit-shr`/`bit-xor` was wrong (those two really are builtins;
`int->float` never was).

**Fix:** added `(defn int->float [x :int] : float 0.0)` to
`turi_env_preload_native_stubs`, next to the already-correct
`int->unit-float`/`tur-sqrt` stubs it should have shipped alongside from the
start. Verified: `monte_carlo_pi.tur`'s `turi` variant now runs to completion
and prints a plausible pi estimate instead of erroring.

## Verification

- `bit-xor`, `int->unit-float`, `tur-sqrt` (the other names preload.c grouped
  with `bit-shr`/`int->float`) were individually checked under `--interpret`
  and are correct -- `bit-xor` is a real builtin (`^`, no sign-extension
  ambiguity to begin with); the other two already had correct stubs and
  natives.
- `bash tests/run.sh` (Debug build): 2746 passed, 5 failed -- the same 5
  pre-existing "no runnable input" fixture-scaffolding failures unrelated to
  this work, present before any of these fixes.
- `bash tests/run-turi.sh`: 1885 passed, 0 failed.
- `TUR=./build-jit/tur bash tests/run-jit.sh`: 2649 passed, 0 failed.
- `performance-comparison` full sweep re-run: `turi` 21/21 (was 20/21 before
  the `int->float` fix, 15/21 before `vec-new-filled`), `sort`'s min/max both
  positive, `monte_carlo_pi` runs to completion. `validate_correctness.py`
  21/21, `aggregate_results.py --baseline rust` clean, `check_reproducibility.py`
  0 entries flagged (CV > 10%). See the B5 note in
  `docs/archive/post-jit-benchmark-resurrection-plan.md` for the full sweep
  numbers.
