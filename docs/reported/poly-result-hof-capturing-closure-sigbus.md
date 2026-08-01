# Capturing closure into a non-fat, non-carrier `(fn ...)` parameter -> runtime crash

**Severity:** medium (miscompile: clean compile, crashes at run time; a
carrier-eligible-signature workaround exists).

**Status:** PARTIALLY RESOLVED 2026-07-30 -- fn-value-fat-normalization
stage 1 landed with a narrowed claim
([docs/upcoming/fn-value-fat-normalization-plan.md](../upcoming/fn-value-fat-normalization-plan.md)).
Of the crash table below: the by-value struct arg/result rows, heap-result
shapes, and the `^linear`/`^borrow` rows are FIXED (concrete effect-free
signatures are fat-normalized; pinned by
`tests/fixtures/fn-value-fat-normalized-params/`).  Still crashing, and
now explicitly out of the narrowed claim: the tyvar arg/result rows
(arguments arrive thin through the generic/carrier machinery) and the
effect-row row (the thin convention is load-bearing for the CPS backend).
The report stays open for those rows.

Root cause identified and mechanism confirmed (2026-07-29);
the fix is a calling-convention change, not a patch -- see
[Investigation](#investigation-2026-07-29). The original title said
"polymorphic-result HOF"; the trigger is materially wider than that, so the
title has been generalized. Everything below the investigation section is the
report as originally filed.

2026-07-30: `tests/type-fuzz-src.py` found the sibling family for fn-typed
VALUES (a returned closure through a pass-through param, `^fat` included, or
an ascription around a let) -- see
[fn-typed-value-return-ascribe-miscompiles.md](../archive/history/fn-typed-value-return-ascribe-miscompiles.md).
Those repros are additional acceptance tests for the calling-convention plan
sketched below; the two reports should land together. The plan is now
written: [docs/upcoming/fn-value-fat-normalization-plan.md](../upcoming/fn-value-fat-normalization-plan.md).

## Investigation (2026-07-29)

### The trigger is not the tyvar result

The report's headline (and its control table) says the trigger is specifically
*result tyvar + capture*. That is one instance of a wider rule. Measured on
`build/tur` at `ae00d40c`, all of these compile clean and then crash
identically (SIGSEGV rc=139 on Linux; the report saw SIGBUS rc=138):

| `(fn ...)` parameter shape, capturing closure passed | result |
| --- | --- |
| concrete `int` result | **OK** |
| concrete `float` result | **OK** |
| `^fat` param | **OK** |
| stored in a `defstruct` fn-typed *field* | **OK** (fixed separately, see below) |
| tyvar **result** -- `(fn [] R)` (the report's case) | **crash** |
| tyvar **argument** -- `(fn [A] int)` | **crash** |
| by-value struct **result**, no tyvar anywhere | **crash** |
| by-value struct **argument**, no tyvar anywhere | **crash** |
| effect row -- `(fn [] #fx{Write} int)`, no tyvar | **crash** |
| `^linear` param | **crash** |
| `^borrow` param | **crash** |
| *non-capturing* closure, tyvar result (control) | **OK** |

So a type variable is neither necessary nor special. The rule is: **a capturing
closure crashes whenever the `(fn ...)` parameter is not routed through one of
the representations that can carry an environment.**

The report's remark that "keeping an argument/phantom type variable is fine" is
consistent with this and does not contradict it -- its example
(`[W] [^borrow t : (Cap W) body : (fn [] int)]`) has a tyvar in a *different*
parameter, leaving `body`'s own signature fully concrete and carrier-eligible.
A tyvar inside `body`'s own signature crashes.

### Mechanism

`src/compiler/elab_fns.c` (`carrier_ok`, ~line 3600) decides per fn-typed
parameter whether it becomes the `tur_poly_fn_t {env, fn}` carrier. It requires
a plain, non-effectful, non-cfnptr, non-variadic signature with no named tyvar
(`fn_type_has_named_tyvar`) and every argument and result a concrete scalar
(`fn_type_is_carrier_safe`). Failing any of those, the parameter keeps its
*nominal bare-function-pointer* `TY_FN` representation.

That representation has nowhere to put a closure environment, and nothing
rejects a capturing closure being passed to it. For the report's repro the
emitted C is:

```c
/* call site (main): builds a fat env object, passes its ADDRESS */
struct __env_1305 *__t159 = ...;          /* { __fn; k; } -- __fn is slot 0 */
__t159->__fn = (tur_thunk_int64_t_t)polybug____fn_1303;
__t159->k    = k_1302;
polybug__run((int64_t)(intptr_t)__t159);

/* callee: calls that ADDRESS as if it were code */
static int64_t polybug__run(int64_t body) {
    return ((int64_t (*)(void))(intptr_t)body)();   /* <-- jumps into the env struct */
}
```

The env layout is self-describing (`__fn` at slot 0), so the correct lowering is
to read slot 0 and pass the object as the environment. The callee never does.
Emitted `polybug__run` is **byte-identical** in the crashing and the
non-capturing control -- the two call sites simply pass values under
incompatible ABIs, and the callee implements only the thin one.

Sites: `src/compiler/emit_expr.c:4536` (nullary nominal-`TY_FN` invoke) and its
n-ary sibling at ~4550.

### The fix is a calling-convention change

Two neighbouring places in the tree already solve exactly this, which is what
makes the direction clear:

- **`:ptr<void>` sinks** (`emit_expr.c:4246`) carry an `is_fat` flag that
  disambiguates thin-vs-fat dispatch at the invoke, "a captureless bare fn
  through a plain `:ptr<void>` stays thin, while a closure/^fat sink dispatches
  fat -- both correct at n==0". The nominal `TY_FN` invoke has no such gate.
- **`defstruct` fn-typed fields** had this identical bug and it was fixed by
  making the representation uniform: see
  `tests/fixtures/capturing-closure-struct-field/` -- "Concrete `(fn ...)`
  fields now use the fat representation uniformly ... the make-struct store
  shims a bare/thin fn into a fat `{thunk, env}` handle, and every field-call
  dispatches via the fat protocol (TUR_APPLY)."

Doing for non-carrier fn-typed *parameters* what was done for fn-typed *fields*
is the principled fix: normalize every value flowing into such a parameter into
a fat `{thunk, env}` handle (shimming bare top-level fns and non-capturing
lambdas on the way in), and dispatch fat at the invoke. It is a calling-
convention change across every non-carrier fn-typed parameter, so it wants its
own plan and a full-suite regen, not a patch.

Note the carrier is **not** the way out: `fn_type_has_named_tyvar` exists
precisely because demoting a named-tyvar fn param onto the carrier "bakes the
int64-carrier ABI and miscompiles a by-value struct argument"
(`poly-hof-constrained-arg-baked-carrier`). Widening carrier eligibility would
reintroduce an already-fixed miscompile.

### A compile-time diagnostic is not the cheap substitute it looks like

Rejecting the unrepresentable combination at elaboration -- turning the crash
into an error -- was tried and abandoned. The predicate
"argument is `EX_CLOSURE` with `n_captures > 0` and the parameter is a nominal
non-fat `TY_FN`" is easy to write and fires on six fixtures that are correct
today (`capturing-closure-struct-field`, `local-struct-fnfield-drop`,
`hkt-cata-fn-arg-carrier`, `van-laarhoven-lens-compose`,
`van-laarhoven-lens-wide-compose`, `lens-compose-wide-byvalue-get-put`).

Refining it by consulting the callee's own param binding (`is_fat` /
`is_poly_fn` / `TY_PTR_VOID`) does not help: the false positives are struct/ADT
*constructor* calls (`Adder`, `Lens` -- whose fields are fat) and higher-order
invocations of *local* fn-typed parameters, neither of which has a
`source_fn_def` to consult. Whether a given closure argument is representable
is settled downstream in emit across five different representations (carrier /
`^fat` / `:ptr<void>`-fat / nominal `TY_FN` / struct-field-fat), and is not
reliably reconstructible at the elaboration call site. A gate that rejects
working programs is worse than the status quo, so nothing was landed.

## Summary (as originally filed)

A **capturing** closure passed to a higher-order function whose result type is
a **type variable** (`(fn [] R) ... : R`) compiles without error and then
crashes at run time (SIGBUS, exit 138). A non-capturing closure is fine, and a
monomorphic result (`(fn [] int) ... : int`) with the same capture is fine --
so the trigger is specifically *result-tyvar HOF + closure environment*.
(Superseded: the trigger is wider than this -- see
[Investigation](#investigation-2026-07-29).)

## Minimal repro

```turmeric
(defmodule polybug (export)
;; Polymorphic HOF whose result is the type variable R.
(defn run [R] [body : (fn [] R)] : R (body))
(defn main [] : int
  (let [k 7]
    ;; Capturing closure (captures k) invoked through run: crashes.
    (let [r (run (fn [] : int (+ k 1)))]
      (do (println r) 0))))
)
```

```
$ tur run polybug.tur   ; compiles clean, then:
rc=138   (SIGBUS), no output
```

## Controls (isolate the trigger)

| variant | result |
| --- | --- |
| result is a tyvar `R`, closure **captures** `k` | **rc=138 SIGBUS** |
| result monomorphic `int`, closure captures `k` (`(defn run [body : (fn [] int)] : int ...)`) | rc=0, prints 8 |
| result tyvar `R`, **non-capturing** closure (`(fn [] : int 42)`) | rc=0, prints 42 |

Keeping an *argument/phantom* type variable is fine; only the **closure
result** being a tyvar triggers it. E.g.
`(defn f [W] [^borrow t : (Cap W) body : (fn [] int)] : int (body))` runs; the
same with `[W R] ... (fn [] R) : R` crashes.

## Where it bites

Discovered building phase B1 of the stateful-refinement plan
(`docs/upcoming/v1/refine-stateful-measures-plan.md`): a `with-frozen` region
combinator wants the natural polymorphic signature
`[W R] [^borrow tok : (DespawnCap W) body : (fn [] R)] : R`, but that shape
crashes with a capturing region body (the common case -- the body reads the
world it closed over). Worked around there by fixing the body result to `int`
(`turmeric-spices/spices/ecs/src/ecs/freeze.tur`), which is why B2's region form
should not be a HOF (or this must be fixed first).

## Likely root cause (direction, not verified)

The crash smells like the fat-closure ABI colliding with the
per-monomorphization result register class: when the HOF's result is a tyvar,
the closure's boxed environment and the erased int64-carrier result path are
reconciled differently than for a ground result, and the env pointer or the
result slot is read at the wrong width/offset. Compare the working monomorphic
path in `emit_expr.c`'s closure-call lowering against the tyvar-result path;
the memory notes on poly-closure-result specialization
(`closure_return_dispatches*` on `Binding`) are the neighbouring machinery.

## Reproduce

`tur run` the snippet above with any current `build/tur` (seen at v0.31.0).
The monomorphic control in the table is the fastest confirmation it is the
result tyvar and not the capture.

## Guide upkeep

When this report is resolved -- or any representation/bridge it describes
changes shape on the way -- update
[docs/guides/value-representations-guide.md](../guides/value-representations-guide.md)
in the same PR: fix the representation inventory, move this report's row out
of the missing-cells table, and correct the link when the report moves to
`docs/archive/`.
