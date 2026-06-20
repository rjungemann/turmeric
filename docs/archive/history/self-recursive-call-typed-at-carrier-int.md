# Self-recursive call result typed at the int64 carrier (`int`) instead of the declared return

**Status:** RESOLVED. Fixed by propagating the declared result shape
(`result_kind` + `result_full_type`) to the forward-decl self-binding in the
Phase HRT5 early-update block (`src/compiler/elab_fns.c`, the new `RR1` block
after the arg-marker propagation), before the body is elaborated. The recursive
self-call now resolves to the declared return, so both `if` arms agree.
Regression fixture: `tests/fixtures/self-recursive-carrier-struct-return/`.
`bash tests/run.sh`: 1717 passed, 0 failed.

**Severity:** medium-high (silently mis-types every self-recursive function
whose return type lowers to the int64 carrier -- `:copy` structs, `(Vec T)`,
handle types -- surfacing as a spurious `if`/branch type mismatch; the only
escape is an explicit ascription at the recursive call).

**Layer:** elaboration / type inference (NOT covered by #459 -- there is no
declared-vs-body conflict here; the declared and produced return types agree,
the recursive *self-reference* is the thing mis-inferred).

## Repro

A1 -- `:copy` struct return:

```turmeric
(defstruct Box :copy [lo : int  hi : int])
(defn go [n : int  i : int  acc : Box] : Box
  (if (>= i n) acc (go n (+ i 1) acc)))   ;; recursive call declared : Box
;; tur check =>
;;   error: if branches have mismatched types: then=Box else=int
;;   (the `else` is `(go ...)`, declared : Box, typed int)
```

A2 -- `(Vec int)` return (same bug, different carrier-lowered type):

```turmeric
(defn go [n : int i : int out : (Vec int)] : (Vec int)
  (if (>= i n) out
    (do (vec-push! out (* i i)) (go n (+ i 1) out))))
;; tur check => then=(type-app Vec int) else=int
```

Control -- a *non-recursive* return of the same struct type-checks (recursion
is the trigger):

```turmeric
(defstruct Box :copy [lo : int  hi : int])
(defn mk [i : int] : Box (make-struct Box i (* 2 i)))
(defn pick [c : bool acc : Box] : Box (if c acc (mk 9)))   ;; OK
```

Verified against a clean Debug build at `b6b532d`: A1/A2 fail, control passes.

## Root cause

`src/compiler/elab_fns.c`, the **Phase HRT5 early-update block**
(`elab_fns.c:2176-2239`). Before the body is elaborated, this block re-stamps
the forward-declared self-binding (`existing->type`, a `TY_FN`) with the
freshly-parsed signature so that recursive self-calls in the body see the real
arity and argument shape. It propagates, in order: `arity`, `arg_kinds`,
`is_variadic` / `rest_kind` / `rest_full_type`, `arg_full_types` (poly),
`arg_poly_fn`, `arg_fat` / `result_fat`, and the substructural markers
(`arg_borrow`/`arg_linear`/`arg_affine`/...).

It never propagates the **result**: `result_kind`, `result_full_type`, or the
struct/ADT decomposition. So a recursive self-call elaborated inside the body
reads `existing->type.as.fn.result_kind`, which still holds the **pass-1
forward-decl placeholder** -- the int64 carrier `TY_INT`. The call expression
is therefore typed `int`, and when it sits in one arm of an `if` whose other
arm is the real `Box` / `(Vec int)`, the branch-unification check reports
`then=Box else=int`.

The data needed is already in scope at line 2176: `return_kind` is computed at
`elab_fns.c:1869-1980`, and `return_struct_def` / `return_adt_def` at
`elab_fns.c:1943-1958`, all *before* the early-update block. The final
`fn_type` is built with the correct result at `elab_fns.c:2876`
(`type_fn(arg_kinds, n_params, return_kind)`) and `result_full_type` is set at
`elab_fns.c:2883-2932` -- but that is too late for the in-body recursive call,
which has already been elaborated against the stale binding.

## Fix direction

In the HRT5 early-update block (`elab_fns.c:2176-2239`), after the arg-side
propagation, also set the result side on `existing->type.as.fn`:

- `result_kind = return_kind;`
- `result_full_type = <the result full Type>` -- mirror the same precedence the
  final `fn_type` uses (`return_struct_def` -> struct full type;
  `return_adt_def` -> `type_adt(...)`; else `return_session_type` /
  `return_tyvar_type` / `return_fn_type` / `return_app_type` /
  `return_exists_type` / `return_borrow_type`). Factoring that precedence into a
  small helper and calling it from both the early-update and the final
  construction (line 2883+) would keep the two in sync.

Then the recursive self-call resolves to the declared return type and both `if`
arms agree. Add fixtures: a self-recursive `:copy`-struct accumulator and a
self-recursive `(Vec int)` accumulator (both should `tur run` clean), plus the
non-recursive control.

**Caveat to check:** the line-2851 fallback (`return_kind == TY_NIL ||
TY_TYVAR` then infer from `body->type`) means an *inferred* (un-annotated)
return is only known after the body. The early-update can only forward an
explicitly-annotated return; for the inferred case the recursive call has no
declared return to forward, so that path should stay as-is (it is the genuinely
mutually-dependent case). The bug here is specifically the *annotated* return
that is already known at line 2176.

**Workaround (spice-side, today):** ascribe the recursive call, e.g.
`(:: (go ...) (Vec int))`. For `(Vec int)` this type-checks and codegens
cleanly (int carrier). For a `:copy`-struct return the ascription type-checks
but then mis-emits (see
`docs/reported/return-struct-param-byval-codegen.md`), so a struct accumulator
must instead thread a raw `ptr<void>` carrier.
