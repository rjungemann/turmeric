# A let-bound NON-capturing lambda segfaults when passed as a `:fn` argument

**Severity: high** -- SIGSEGV in compiled code, no diagnostic; the interpreter
got it right, so the two paths disagreed.
**Status: RESOLVED.**

Found while scoping F5 (callbacks) of jit-ffi-c2mir-plan -- it is why that
phase does not build a trampoline on top of the compiled closure
representation.

## Repro

```turmeric
(defn takes [f : (fn [int] int)] : int (f 7))
(defn main [] : int
  (let [g (fn [x : int] : int (* x 2))]
    (println (takes g)))
  0)
```

`./build/tur run` exited 139 with no output; `--interpret` printed 14.

## Root cause

A `:fn` value has **three** lowerings, and a `tur_poly_fn_t` consumer always
calls `f.fn(f.env, args...)`:

| lowering | representation | worked? |
|---|---|---|
| capturing lambda | a box; slot 0 is the code pointer | yes |
| inline lambda | a `__poly_` wrapper with the env parameter spliced in | yes |
| let-bound, no captures | the raw `int64_t (*)(int64_t)` -- **no box** | SIGSEGV |

`src/compiler/elab_call.c` routes any *local* fn binding to the "is_closure"
emit path, whose comment asserted that path "reads the thunk from the box's
slot 0 at runtime, so both a capturing closure and a passed-in fn round-trip".
True for those two; the third has no box. So
`src/compiler/emit_expr.c` emitted

```c
takes((tur_poly_fn_t){ __t160, (int64_t(*)(void*,int64_t))(intptr_t)((int64_t*)__t160)[0] });
```

where `((int64_t*)__t160)[0]` read the first eight bytes of the function's own
machine code and called the result. The fault is the jump, not the load.

## Resolution

Fix direction (1) from the report -- keep exactly one consumer contract --
implemented without changing how lambdas are lowered:

- `ensure_bare_fnptr_poly_shim` (src/compiler/emit_module.c) emits a
  signature-keyed file-scope adapter
  `static R __tur_barefn_<sig>(void *__e, A0 a0, ...)` that casts `__e` back
  to the **env-less** signature and calls it with the arguments only.
- The materialization site carries the bare pointer in the `env` slot -- where
  it fits, being pointer-sized -- and pairs it with that adapter, so the
  consumer's `f.fn(f.env, args...)` reaches the function correctly.

The shim references no local, so unlike `make_poly_wrapper` (whose `__poly_N`
statically names its inner function, and which is exactly why local bindings
were excluded from that path) it is legal at file scope.

**The discriminator** is the one thing that separates the three shapes: an
**unboxed** `TY_FN` binding. A capturing closure is boxed or `:ptr<void>`; a
forwarded fn parameter arrives as a `tur_poly_fn_t` and is boxed too. Verified
by the seven `van-laarhoven-lens-*` fixtures, which are the legitimate users of
the slot-0 read -- they still take it and still pass, so the new branch fires
only on the shape that was broken.

## Blast radius

**Zero.** run.sh 2672 passed / 0 failed, run-turi.sh 1843 passed / 0 failed,
and **no `expected.c` snapshot moved** -- the adapter is emitted only where the
old code would have faulted.

## The report's follow-up question

> Worth checking whether the same three-way split affects storing a
> non-capturing lambda in a struct field, a vector, or a global.

Checked. A bare lambda stored in a `defstruct` field and read back
(`(takes (.cb hd))`) faulted the same way and is fixed by the same adapter --
the fix is at the `:fn`-argument boundary, so it covers every route a bare
pointer takes to get there. That case is in the fixture (row 4).

## Tests

`tests/fixtures/fn-arg-let-bound-noncapturing` -- all three lowerings in one
file, so a future change to fn lowering cannot fix one and break another, plus
the struct-field round-trip, two distinct bare lambdas in one scope (distinct
adapters), and one binding passed twice (shim reused, not re-emitted). Runs on
both the compiled and interpreted harnesses, which now agree line for line.
