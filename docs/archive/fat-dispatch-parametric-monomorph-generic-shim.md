# A parametric ADT monomorph returned through a fat boundary gets the generic int64 shim

**Severity: high.** A silent SEGV on the **default path** -- no seam, no
experiment, no flag. The two narrow widths return the right answer by
register-class luck, so the bug is invisible until an aggregate crosses the
SysV 16-byte threshold, at which point the program jumps to address 0.

**Status: RESOLVED 2026-08-27.** One-line predicate fix plus one lockstep
repair the fix exposed -- see **Resolution**.

Found 2026-08-27 while working the M7 HKT-carrier worklist from the
[SR2 prototype gate](../upcoming/sr2-gate-results.md), whose acceptance test
(`parsec-tutorial` builds under the SR2 seam and then segfaults) turned out to
be this bug wearing a seam it did not need.

**It is the fourth member of the family whose other three are fixed:**

- [fat-closure-dispatch-does-not-handle-struct-return](history/fat-closure-dispatch-does-not-handle-struct-return.md)
  -- RESULT position, non-parametric aggregate.
- [poly-wrapper-forces-int64-args-non-int-fat-sink](history/poly-wrapper-forces-int64-args-non-int-fat-sink.md)
  -- ARGUMENT position, `:float`.
- [fat-dispatch-wide-byvalue-aggregate-argument](fat-dispatch-wide-byvalue-aggregate-argument.md)
  -- ARGUMENT position, wide by-value aggregate.
- **This one** -- RESULT position, PARAMETRIC aggregate (`(Box2 int)`).

Same one-sentence pattern as the other three: **a lifted thunk is declared with
the concrete C signature, and the dispatch site casts it to something else.**

## Minimal repro (default path, no flags)

```turmeric
(defdata Big3 :copy [a] (Big3 [x : a y : a z : a]))

(defn mk3 [n : int] : (Big3 int) (Big3 n (* n 2) (* n 3)))

(defn apply3 [^fat f : (fn [int] (Big3 int)) n : int] : int
  (match (f n) (Big3 x y z) (+ x (+ y z))))

(defn main [] : int
  (println (apply3 mk3 5))   ; expect 30
  0)
```

Builds clean, then SEGVs. Reproduced at the pre-session merge base
`2c869636`, so it predates all SR work.

Narrow the payload and the bug hides:

| repro | aggregate | return convention | result |
|---|---|---|---|
| `(Box2 [v : a w : a])` over `int` | 16 bytes | RAX:RDX | 77 -- **correct** |
| `(BoxF [v : a w : a])` over `float` | 16 bytes | XMM0:XMM1 | 78.1 -- **correct** |
| `(Big3 [x y z])` over `int` | 24 bytes | sret (hidden pointer) | **SEGV** |

## Root cause

`thunk_type_has_concrete_c_abi` (src/compiler/emit_module.c) answered for
`TY_ADT` but had no `TY_APP` case, so it fell to `default: return false`. A
concrete parametric monomorph therefore reported "no C ABI",
`use_typed_thunk_abi` declined the typed fatshim, and `EX_FN_TO_FAT` left slot
0 of the fatbox holding the generic `__tur_fatshim<arity>`:

```c
int64_t __tur_fatshim1(void *__e, int64_t a0);
```

The dispatch site then casts that pointer to the aggregate-returning
signature:

```c
tur_adt_PRes__Expr __ps_57 =
    ((tur_adt_PRes__Expr (*)(void *, int64_t))(intptr_t)((int64_t *)env)[0])(env, xs);
```

A function-pointer cast is exactly the construct cc cannot diagnose, which is
why this survived as long as it did -- and why it survived *correctly* at the
narrow widths. The generic shim is a **transparent forwarding tail-call**:
whatever registers the real function reads and writes pass through it
untouched, so an aggregate returned in registers came back intact despite the
declared `int64_t`. Past 16 bytes SysV switches the return to the hidden-pointer
(sret) convention, which shifts **every argument right by one slot**. The shim
then reads the caller's sret destination as its env, loads a function pointer
out of it, and jumps to whatever that garbage says -- 0, in practice.

## Resolution

**The fix**, in `thunk_type_has_concrete_c_abi`:

```c
case TY_APP:
    return type_app_is_concrete_adt(&t);
```

The trap worth recording: the obvious-looking predicate,
`type_has_concrete_codegen_layout`, returns false for **every** `TY_APP` by
design -- its struct-app branch defers to `type_extract_struct_app`, and its
own comment names `type_app_is_concrete_adt` as the parametric-ADT answer.
Gating on it reads as "no parametric app ever has a C ABI" and silently
reinstates the bug; the first attempt at this fix did exactly that and changed
nothing.

**The lockstep repair the fix exposed.** Selecting the typed thunk for a
`TY_APP` result made a pre-existing structural hazard reachable: an env
struct's `__fn` field is declared once, at whichever site emits the struct
first, but is ASSIGNED at every closure-construction site -- and those sites do
not resolve the thunk result identically. A generic site resolves a tyvar
result to the int64 carrier; a monomorphising site (`..__byval`, `..__spec__..`)
resolves the same result to a concrete aggregate typedef. Each site recomputed
the spelling, so the two contradicted each other and the assignment stored a
function pointer through an `int64_t` field:

```c
struct __env_1385 { int64_t __fn; ... };
__t272->__fn = (tur_thunk_tur_adt_Identity__Point_tur_adt_Point___t)__fn_1383__byval;
/* warning: assignment to 'int64_t' from '...(*)(void *, tur_adt_Point *)'
   makes integer from pointer without a cast */
```

Fixed the way the sibling report's argument-position bug was: one source of
truth. `ctx->env_struct_fn_typedefs` records the spelling each env struct was
actually DECLARED with, both emit sites (emit_expr.c and the emit_fns.c
pre-pass twin) register through `emit_env_struct_register`, and the assignment
site reads it back with `emit_env_struct_fn_typedef` instead of recomputing.

## Fixtures

`tests/fixtures/fat-dispatch-parametric-monomorph-return` pins all three
widths and **checks values**, not just that it builds -- the whole failure
class hides behind a function-pointer cast, so a build-only assertion would
have passed throughout. Also added to `tests/run-sr4-seam.sh`.

Four `van-laarhoven-lens-wide-*` snapshots regenerated in the same change:
they now select the typed fatshim where they previously carried
`__tur_fatshim1`.

Full suite after: **2710 passed, 0 failed**; sr4-seam **24 passed, 0 failed**.

## What it does NOT close

The other ten fixtures on the SR2 gate's M7 worklist are unchanged -- they are
a separate carrier/elaboration disagreement (`(ExprF (fn [int] int))` assigned
into an int64 slot, "aggregate value used where an integer was expected"), not
an ABI-selection hole. See
[sr2-gate-results.md](../upcoming/sr2-gate-results.md).
