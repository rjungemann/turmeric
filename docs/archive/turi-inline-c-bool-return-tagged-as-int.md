# turi: an inline-C body declared `: bool` hands back a `TURI_INT`

**Summary:** Under `tur --interpret` and at the `tur repl` prompt, a `defn`
whose body is an inline-C block and whose declared result is `: bool` returns
its C result as a plain `TURI_INT`. The truth value is right, but the *tag* is
wrong, so everything that dispatches on the tag diverges from the compiled
program: `println` prints `1`/`0` where the compiled binary prints
`true`/`false`, `type-of` (via `any`) answers `int` instead of `bool`, and a
`match` on the value finds no `true`/`false` arm and **aborts the program**.
Found while checking the AOT-compiled REPL plan; first seen on a character
check, `(is-close 41)`, echoing `=> 1`.

**Severity:** Medium. The `println` half is cosmetic, but the `match` half is a
runtime abort (`tur: eval: match: no arm matched`, rc=1) on a program that
runs cleanly when compiled, and the `type-of` half feeds the `any` surface the
wrong static name. Every stdlib inline-C predicate is shielded by the native
override (`interpreter_natives.c` registers a real native under the same name,
which wins at `src/turi/eval.c` ~9895), so this bites **user** inline-C only --
which is exactly the code the REPL and `--interpret` exist to try out.

**Repro** (Linux x86-64, `main` @ 81e12de4, v0.56.3):

```turmeric
(defn yes [] : bool
  ```c
  return 1;
  ```)
(defn is-close [c : int] : bool
  ```c
  return c == 41;
  ```)
(defn main [] : int
  (println (yes))                              ; interp: 1     compiled: true
  (println (is-close 41))                      ; interp: 1     compiled: true
  (println (is-close 7))                       ; interp: 0     compiled: false
  (println (if (yes) "taken" "not-taken"))     ; both: taken   (truthiness is fine)
  (println (not (yes)))                        ; both: false
  (println (= (yes) true))                     ; both: true
  (println (type-of (:: (yes) any)))           ; interp: int   compiled: bool
  (println (match (yes) true "t" false "f"))   ; interp: ABORT "match: no arm matched"
  0)                                           ; compiled: t
```

```
$ ./build/tur --interpret yes.tur      # prints 1 1 0 ... then "tur: eval: match: no arm matched", rc=1
$ ./build/tur build yes.tur -o yes && ./yes   # prints true true false ... t, rc=0
$ printf '(defn yes [] : bool\n```c\nreturn 1;\n```)\n(yes)\n' | tur repl
=> #<fn yes>
=> 1                                  # expected => true
```

`tests/fixtures/ascribe-bool-to-numeric-prints` is the neighbour (a bool
*ascribed* to int, resolved 2026-08-06 at the render site); this one is the
opposite direction -- a C int that was never re-tagged to the declared bool.

## Root cause

`src/turi/eval.c`, the simple inline-C executor's "simple-return" pattern
(Pattern 7, ~line 5993):

```c
if (ic_eval_assign_expr(r, fn, param_offset, args, n_args, &val, body)) {
    TuriValue rv={0}; rv.tag=TURI_INT; rv.as_int=val; *out=rv;
    return ic_claim("simple-return", fn, out);
}
```

The tag is hard-coded `TURI_INT` regardless of `fn->return_type.kind`. The
call site that consumes `try_exec_simple_inline_c` (`eval.c` ~9923-9960)
re-tags exactly one shape -- a non-zero `TURI_INT` for a declared ADT/struct
result becomes a `TuriStruct*` -- and has no `TY_BOOL` arm. `ic_exec_accessor`
(Pattern 5) has the same gap for a `return p->flag;` with a `: bool` result.

The interpreter already knows the right fix in two other places:

- the extern-c thunk path, `eval.c:600` and `:2244`:
  `case TY_BOOL: result = turi_bool(out_i != 0); break;`
- the typeclass dispatch path, `eval.c:~12416`, which re-tags an inline-C
  instance body declared `: bool` (`Eq [cstr]`'s `strcmp`) from `TURI_INT`
  with the comment "give it the kind the CLASS declares".

That second site is why `(= (yes) true)` prints `true` on both paths: the
`Eq [bool]` dispatch coerces the operand, masking the tag on that one route.

## Fix directions

1. **Re-tag at the single seam** (preferred, ~5 lines): in the
   `try_exec_simple_inline_c` consumer next to the ADT/struct re-tag, add
   `if (inline_result.tag == TURI_INT && fn->return_type.kind == TY_BOOL)
   inline_result = turi_bool(inline_result.as_int != 0);`. Covers every
   pattern the executor claims (simple-return, accessor, constructor) in one
   place. Check that `fn->return_type.kind` is populated for a plain `defn`
   (the ~12416 comment notes it is *not* for a typeclass method; the
   binding's `type.as.fn.result_full_type` is the fallback used for the
   opaque check in the same block).
2. Same rule for `TY_NIL`/`unit` (a `return 0;` under `: unit` should be
   `turi_nil()`) and `TY_CSTR` (a returned string literal), if the executor
   can produce them -- audit the claim sites rather than assume.
3. Pin with a fixture: `tests/fixtures/inline-c-bool-return-prints`, plain
   `run.sh` (no `requires.*`), holding the repro above minus the `type-of`
   line, so both paths must print `true`/`false`/`t`; and its `run-turi.sh`
   counterpart asserting `=> true` at the prompt.

Not a fix: converting inside `println`'s tag dispatch, as
`ascribe-bool-to-numeric-prints` did -- that repaired one renderer and would
leave `match` and `type-of` wrong here.

## Resolution (2026-09-30)

Fixed by direction 1, at the single seam: the consumer of
`try_exec_simple_inline_c` in `src/turi/eval.c` now re-tags a `TURI_INT` result
as `turi_bool(v != 0)` when the declared result is `bool`. It reads
`fn->return_type.kind` (populated for a plain `defn`) and falls back to the
binding's `result_full_type`, the same source the opaque check in that block
already uses. Every pattern the executor claims (simple-return, accessor,
constructor) goes through that one site.

Direction 2 (`unit` / `cstr`) did not need a change: `unit` is not a result
type a `defn` can declare, and no claim site hands back a string as an int.

Pinned by `tests/fixtures/inline-c-bool-return-prints`, which is on
`run-turi.sh`'s `TURI_INLINEC_RUN` allowlist so the interpreter runs it too
(the inline-C carve-out would otherwise skip it). Both paths print
`true`/`true`/`false`, `type-of` answers `bool`, and both `match`es find their
arm. `printf '(defn yes [] : bool ...)\n(yes)\n' | tur repl` echoes `=> true`.
