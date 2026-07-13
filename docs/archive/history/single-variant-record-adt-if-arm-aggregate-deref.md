# Single-variant record ADT ctor as an `if`/`cond` arm is deref'd as an int handle (miscompile)

**Summary:** When a single-constructor ("record") ADT carries a heap ADT field,
using its constructor as one arm of an `if`/`cond` whose merge temp is by-value
(e.g. the other arm calls a `defn` that returns the same record) miscompiles.
The constructor emits its value by value (`ctor_<Name>(...)` returns the
aggregate), but `emit_if_value`'s by-value carrier-bridge treats the ctor arm as
an int64 carrier handle and derefs it:
`(*(tur_adt_Wrap *)(intptr_t)(ctor_Wrap(...)))`. The C compiler then rejects it
with **"aggregate value used where an integer was expected."**

**Severity:** Medium. Purely a compile-time break (no silent bad codegen -- the
C compiler catches it), but it blocks an idiomatic pattern: a positional record
ADT (`(defdata Wrap (MkWrap :Box :int))`) whose field is any heap value (another
ADT, a `:copy` recursive ADT, etc.), returned from an `if`/`cond`. The
pure-Turmeric regex port (`stdlib/re.tur`) hit exactly this and worked around it
by giving its `RxPair`/`RxParse` wrappers a dummy second variant to force the
tagged-heap-handle representation.

**Status:** FIXED 2026-07-13 (`src/compiler/emit_expr.c`,
`call_construct_emits_byval_aggregate`). Report archived alongside the fix.

**Minimal repro:**

```turmeric
(defdata Box :copy (Boxed :int) (Empty))
(defdata Wrap (MkWrap :Box :int))          ; single ctor, heap ADT field

(defn helper [b : Box n : int] : Wrap (MkWrap b n))

(defn build [b : Box n : int] : Wrap
  (if (> n 0)
    (MkWrap b n)          ; ctor arm  -> WRONGLY deref'd
    (helper b 0)))        ; call arm  -> correctly assigned by value

(defn get-n [p : Wrap] : int (match p (MkWrap b n) n))
(defn main [] : int (println (get-n (build (Boxed 7) 42))) 0)
```

Emitted C before the fix (`build`):

```c
static tur_adt_Wrap ctor_MkWrap(int64_t _0, int64_t _1) { ... }   // returns BY VALUE
static tur_adt_Wrap build(int64_t b, int64_t n) {
    tur_adt_Wrap __t174;
    if ((n) > (INT64_C(0))) {
        __auto_type __ps_175 = (ctor_MkWrap(b, n));               // __ps_175 : tur_adt_Wrap (value)
        __t174 = (*(tur_adt_Wrap *)(intptr_t)(__ps_175));         // BUG: derefs the value as a handle
    } else {
        __auto_type __ps_176 = (helper(b, INT64_C(0)));
        __t174 = __ps_176;                                        // correct: direct assign
    }
    return __t174;
}
```

**Root cause:** `emit_if_value` (`src/compiler/emit_expr.c`) declares the merge
temp by value when either arm produces a by-value carrier aggregate
(`fn_body_tail_byvalue_carrier_type` non-UNKNOWN -- here recovered from the
`helper` call arm). It then bridges each arm that does *not* already emit the
by-value aggregate, via `!fn_body_tail_emits_byvalue_carrier_abi(arm)`. The same
by-value merge temp + per-arm bridge logic drives `let`-binding initialisers and
match arms. Two gaps in the tail predicates conspired:

1. **Bare `TY_ADT` constructors were not recognised as by-value producers.**
   `fn_body_tail_emits_byvalue_carrier_abi` delegates constructor tails to
   `call_construct_emits_byval_aggregate`, whose gate matched only a concrete
   parametric application:

   ```c
   r.kind == TY_APP && type_app_is_concrete_adt(&r) && ...
   ```

   A **non-parametric** ADT such as `Wrap` resolves to `TY_ADT`, not `TY_APP`,
   so a direct ctor arm was classified as a carrier handle and deref'd. (The
   `TY_APP`-only gate came from the Option/Result carrier work, where every
   by-value ADT of interest was a parametric monomorph like `(Option int)`; the
   bare-`TY_ADT` record case was never wired in.) This is the `if`-arm repro
   above.

2. **The tail predicates did not descend into `EX_MATCH`.** Both
   `fn_body_tail_emits_byvalue_carrier_abi` and `fn_body_tail_byvalue_carrier_type`
   recursed through `EX_IF` / `EX_LET` / `EX_DO` but not `EX_MATCH`. So when a
   branch's tail is itself a `match` (e.g. `stdlib/re.tur`'s `re-parse-brace`,
   whose outer match arm is an `if` whose else is a nested
   `(match (re-read-int ...) (RxIP m p2) ...)`), the predicate fell through to
   the call checks, matched nothing, and the nested match's already-by-value
   result was deref'd.

**Fix (both parts):**

- Extend `call_construct_emits_byval_aggregate` to accept a concrete bare
  `TY_ADT` (def != NULL) alongside the parametric app, keeping the not-carrier /
  not-heap / real-aggregate-c-name guards (and a `T *` suffix guard) so heap
  ADTs and open type variables stay excluded. This also covers fieldless value
  enums used the same way:

  ```c
  ((r.kind == TY_APP && type_app_is_concrete_adt(&r)) ||
   (r.kind == TY_ADT && r.as.adt_.def != NULL)) &&
  !type_uses_carrier_abi(r) &&
  !type_is_heap_struct(r) && !type_is_heap_adt(r) &&
  cn && strcmp(cn, "int64_t") != 0 && cnlen > 0 && cn[cnlen - 1] != '*'
  ```

- Add an `EX_MATCH` case to both `fn_body_tail_emits_byvalue_carrier_abi` and
  `fn_body_tail_byvalue_carrier_type`, mirroring the `EX_IF` case: a match's tail
  value is one of its arm bodies, so it emits (has the type of) a by-value
  aggregate iff some arm's tail does.

Validated: the `if` and `match` repros compile and run; `stdlib/re.tur` works
with no workaround; `bash tests/run.sh` = 2124 passed / 0 failed and
`bash tests/run-turi.sh` = 1595 passed / 0 failed.

**Follow-up applied:** `stdlib/re.tur` dropped its dummy-variant workaround --
`RxPair` and `RxParse` are back to single-constructor records now that the
codegen handles them.
