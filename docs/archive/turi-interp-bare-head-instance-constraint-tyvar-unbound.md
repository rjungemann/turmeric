# turi interpreter: bare-head constrained instance leaves its element tyvar unbound

**Severity:** medium (interpreter-parity gap; compiled path is correct).

**Status: RESOLVED.** At instance-method dispatch the interpreter now binds a
bare-head constrained instance's constraint tyvars from the receiver argument's
static type, mirroring the compiled path's per-call-site specialization. New
helper `frame_bind_instance_constraint_tyvars` (`src/turi/eval.c`) runs right
after `frame_record_abi` in the `DK_CALL_ARG` handler: for a callee whose
`FnDef.owner_instance` carries `type_param_constraints`, it extracts the
receiver arg's ADT-app type-args (`type_extract_adt_app`) and pins each
constraint's `tyvar -> type_args[param_idx]` onto the instance body frame (the
same `param_idx` convention the shared emit kernel
`emit_abi_constraint_var_bindings` uses). The nested repr now prints `1107`
under `--interpret`, matching the compiled path, so the fixture's
`requires.compiled` marker was removed and `tests/run-turi.sh` exercises it.

## Summary

Under `tur --interpret`, a typeclass method dispatched to a *bare-head*
constrained instance -- e.g. `(definstance Tag [Cons] [(Tag A)] ...)` -- runs
its body with the instance's element tyvar `A` unbound. Any nested typeclass
dispatch inside that body that keys off `A` therefore falls back to the
int-carrier representative instance instead of the element's real instance.

The compiled path is unaffected (it specializes each dispatch site and binds
`A` from the receiver's static type), so the same program compiles and runs
correctly.

## Minimal repro

`tests/fixtures/constrained-generic-nested-container-element-dispatch/input.tur`:

```turmeric
(defclass Tag [a] (tag [x] : int))
(definstance Tag [int] (tag [x] 7))
(definstance Tag [(Option A)] [(Tag A)]
  (tag [x] (if (.is-some x) (+ 100 (tag (.value x))) 0)))
(defn tag-head [A] [(Tag A)] [c : (Cons A)] : int
  (tag (:: (.head c) A)))
(definstance Tag [Cons] [(Tag A)]
  (tag [x] (+ 1000 (let [c (:: x (Cons A))] (tag-head c)))))

(defn main [] : int
  (let [nested (:: (list (:: (some 5) (Option int))) (Cons (Option int)))]
    (println (tag nested)))            ; want 1107, interp prints 1007
  0)
```

- Compiled (`tur build` / `tests/run.sh`): prints `1107`. Correct.
- Interpreter (`tur --interpret` / `tests/run-turi.sh`): prints `1007`. Wrong.

## Root cause

`src/turi/eval.c`, generic-dict-dispatch re-resolution.

The interpreter re-resolves a baked-representative typeclass method two ways:

1. Statically, from the receiver tyvar pinned onto the call frame
   (`frame_record_abi` -> `frame_lookup_tyvar` -> `gde_reresolve_method`,
   around `src/turi/eval.c:5491`).
2. By the receiver's runtime value tag (`gde_reresolve_method_by_value`,
   `src/turi/eval.c:6324`), which deliberately bails for a `TURI_INT` carrier
   because Vec/Map/Option-box all ride the int carrier ambiguously.

For the nested case both fail:

- The Cons instance is `[Cons] [(Tag A)]`. Its element tyvar `A` lives only in
  the `[(Tag A)]` constraint, **not** in `TypeClassInstance.type_args` (the head
  is the bare `Cons`). When `(tag nested)` dispatches to `__inst_Tag_tag_Cons`,
  the call's abi-bindings pin only the class param `a = Cons`; nothing binds
  `A = (Option int)`. Frame-pointer tracing confirms the Cons instance body
  frame has `a -> Cons` and no `A`.
- Inside the body, `(tag-head c)` tries to thread its own `A` from the caller
  frame (`frame_record_abi` resolves a TY_TYVAR binding through the caller), but
  the caller frame has no `A`, so the binding stays abstract and is skipped.
- Inside `tag-head`, `(tag (:: (.head c) A))` looks up `A` in the frame,
  misses, and keeps the baked `int` representative -> `Tag[int]` -> `7`, giving
  `1000 + 7 = 1007`.
- The by-value fallback can't rescue it: `(some 5)` is a scalar Option, which
  the interpreter represents as an int64-carrier box (`native_some`,
  `src/main.c`), so the receiver tag is `TURI_INT` and `gde_reresolve_method_by_value`
  bails by design.

A direct `(tag (:: (some 5) (Option int)))` dispatches correctly (`107`) -- the
gap is specific to recovering the element type of a *bare-head* constrained
instance through the receiver's static type.

## Fix directions

Bind a bare-head constrained instance's constraint tyvars at dispatch by
unifying the instance's constraint head `(Tag A)` / the method body's
`(Cons A)` ascription against the receiver argument's static type
`(Cons (Option int))`, then record `A -> (Option int)` onto the instance body
frame (the same slot `frame_record_abi` writes). This mirrors what the compiled
path's per-call-site specialization already does. Needs: the instance's
constraint tyvar names (`TypeClassInstance.type_param_constraints` /
`constraints`), the receiver arg's static type at the dispatch site, and a small
unify step -- none of which the runtime re-resolution currently consults.

## Status

Fixture carries `requires.compiled` so `tests/run-turi.sh` PASS-skips it while
the compiled suite still exercises it. Sibling interpreter bugs surfaced
alongside this one -- the `list-concat` `native_list_length` crash, `some`/
`unwrap` of a heap payload, and the applied-unary Option instance -- are fixed;
this bare-head constraint-tyvar case is the remaining gap.
