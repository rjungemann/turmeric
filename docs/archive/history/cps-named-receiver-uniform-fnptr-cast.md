# CPS marshal-reset named receiver is called through a uniform `int64_t(*)(int64_t)` fn-ptr cast (UB)

> **Status:** RESOLVED 2026-07-19. `emit_cl_shift_bodyfn`'s named-receiver branch
> (`src/compiler/emit_cps_ir.c`) now keys the callee function-pointer type AND its
> argument cast on the same `t->as.cloneable.serial` bit the closure branch uses
> two cases up: a serial receiver emits `((int64_t (*)(void *))...)((void *)...)`
> (matching its real `int64_t rt(void *)` signature) and a cloneable receiver keeps
> `int64_t (*)(int64_t)` (byte-identical to before -- zero fixture-snapshot churn).
> Those two families -- serial `ptr<void>` `k` and cloneable int64_t-carried `k`,
> both returning the int64_t carrier -- are the only named-receiver shapes the type
> contract emits today, so keying on `serial` removes the mismatch for every
> currently-emittable receiver; the fuller `R (*)(A...)` reconstruction the report
> proposes would only matter for a hypothetical future receiver with a new param or
> return type and is deliberately left unbuilt.
>
> **Verification.** The minimal repro's emitted call now casts through
> `int64_t (*)(void *)` (matching `rt5`'s forward-declared `int64_t rt5(void *)`)
> and still prints `15` compiled and interpreted; all 26 serial/cloneable/cont
> fixtures pass. NOTE: this container has no working `-fsanitize=function` runtime
> (gcc lacks the check; clang's ubsan runtime is not installed), so the UBSan
> *trip* itself was not re-run -- verification is the exact signature match in the
> emitted C plus behavioral parity, which is sufficient because the fix makes the
> cast type identical to the callee's own forward declaration.

**Summary:** The native cloneable/serial (`build_marshal_reset` /
`emit_cloneable`) shift-body emitter invokes a *named* continuation receiver by
casting its function pointer to a fixed `int64_t (*)(int64_t)` signature,
regardless of the receiver's real C signature. For a serial receiver typed
`(fn [k : ptr<void>] : int)` -- whose true C signature is `int64_t rt(void *)` --
the call is made through an incompatible function-pointer type, which is
undefined behavior in C. It works on every currently-supported ABI (x86-64 SysV,
AArch64 AAPCS) because `void *` and `int64_t` are passed identically in an
integer register, and it emits **no diagnostic**, so the mismatch is silent.

**Severity:** Low. Works on all supported targets; pre-existing (predates the
marshal-reset unification -- the unification did not introduce or widen it).
Silent (no `-Wint-conversion`, because a function-pointer cast is explicit). The
risk is latent: a target ABI that passes `void *` and `int64_t` differently, or a
CFI / strict-UB sanitizer build (`-fsanitize=function`, which checks the
call-through-pointer signature), would trip on it.

**Minimal repro:**

```turmeric
(load "stdlib/workflow.tur")
(defn rt5 [k : ptr<void>] : int (resume-cont! (save-cont! k) 5))
(defn run [] : int (serial-reset (+ 10 (serial-shift rt5 0))))
(defn main [] : int (println (run)) 0)   ; prints 15, correct
```

Emitted C -- the receiver's real signature vs the call through the cast:

```c
static int64_t rt5(void *);                              // true signature: (void*) -> int64_t
...
// in the dk_shift body fn:
return (intptr_t)((int64_t (*)(int64_t))(intptr_t)env)((int64_t)(intptr_t)__cap);
//                ^^^^^^^^^^^^^^^^^^^^^^ calls rt5 through (int64_t)->int64_t -- signature mismatch
```

**Root cause:** `emit_cl_shift_bodyfn`, named-receiver branch,
`src/compiler/emit_cps_ir.c:3118`:

```c
buf_printf(ce->helpers,
    "static intptr_t %s(intptr_t env, DK *subk) {\n%s"
    "    return (intptr_t)((int64_t (*)(int64_t))(intptr_t)env)((int64_t)(intptr_t)%s);\n}\n",
    bodyfn, cont_setup, cont_arg);
```

`env` carries the receiver's function pointer (threaded through the `dk_shift`
env). It is blanket-cast to `int64_t (*)(int64_t)` and called, so a receiver whose
`k` param is `ptr<void>` (serial) -- or any non-`int64_t`-carried continuation
type -- is invoked through a mismatched signature. The same uniform-cast idiom
appears at the arithmetic-frame wrapper and the reset-site named call (the closure
branch, by contrast, was tightened in the closure-receiver-parity slice to cast
each argument to the thunk's actual param type -- see
`docs/upcoming/v2/cps-backend-unification-marshal-reset-unification-plan.md`).

**Fix directions:**

- Cast through the receiver's *actual* signature rather than a uniform one:
  synthesize the correct `R (*)(A...)` type from the receiver binding's
  `type.as.fn` (result + param kinds) -- the same information the receiver's own
  forward declaration is emitted from -- and cast `env` to that before calling.
  This is the direct analog of the closure-branch fix already landed in
  `emit_cl_shift_bodyfn` (which keys the continuation-arg cast on the family /
  param type); the named branch just needs the same treatment for the *callee*
  pointer type.
- Cheaper interim: since the divergent case is specifically the serial
  `ptr<void>` continuation, cast the fn ptr to `int64_t (*)(void *)` when
  `t->as.cloneable.serial` and keep `int64_t (*)(int64_t)` otherwise -- mirroring
  the `kty` selection already used two lines up in the closure branch. Removes the
  UB for the one shape that actually diverges today without a full signature
  reconstruction.

Not urgent for v1 (correct on all supported ABIs), but worth closing before any
`-fsanitize=function` / CFI-hardened build, and it is a small, localized change.
