# A let-bound NON-capturing lambda segfaults when passed as a `:fn` argument

**Severity: high** -- SIGSEGV in compiled code, no diagnostic. The
interpreter gets it right, so the two paths disagree.

Found while scoping F5 (callbacks) of
[jit-ffi-c2mir-plan](../upcoming/jit-ffi-c2mir-plan.md) -- it is why that
phase does not build a trampoline on top of the compiled closure
representation. Unrelated to jit-ffi otherwise.

## Repro

```turmeric
(defn takes [f : (fn [int] int)] : int (f 7))
(defn main [] : int
  (let [g (fn [x : int] : int (* x 2))]
    (println (takes g)))
  0)
```

```sh
./build/tur run /tmp/m1.tur   ; exit 139 (SIGSEGV), no output
./build/tur --interpret /tmp/m1.tur   ; 14
```

## The boundary is narrow, which is what makes it easy to miss

Three variants, same `takes`:

| variant | compiled | interpreted |
| --- | --- | --- |
| let-bound **non-capturing** lambda (above) | **SIGSEGV** | 14 |
| the same lambda passed **inline**: `(takes (fn [x : int] : int (* x 2)))` | 14 | 14 |
| let-bound **capturing** lambda: `(let [n 3 h (fn [x : int] : int (+ x n))] (takes h))` | 10 | 10 |

So it needs all three of: a lambda, bound to a local, with no captures.
Adding a single captured variable makes it work.

## Root cause

A `:fn` value's canonical representation is the int64 carrier pointing at a
closure BOX whose slot 0 is the code pointer and whose own address is the
env. The call-argument path materializes `tur_poly_fn_t` from that
assumption. From the emitted C (`tur emit-c` on the repro):

```c
/* the let binding -- a bare function pointer, NOT a box */
int64_t (*g)(int64_t) = (int64_t (*)(int64_t))(intptr_t)(__fn_1329);
...
void *__t163 = (void *)(intptr_t)(g);
takes((tur_poly_fn_t){ __t163, (int64_t(*)(void*,int64_t))(intptr_t)((int64_t*)__t163)[0] });
                                                          /* ^ reads slot 0 */
```

`((int64_t*)__t163)[0]` reads the first eight bytes of the FUNCTION'S CODE
and calls the result. On a non-executable-data platform that faults
immediately; the observed SIGSEGV is the jump, not the load.

The capturing variant works because it really does allocate a box:

```c
void *__t161 = malloc(sizeof(void *) + sizeof(struct __env_1336));
*(void (**)(void *))__t161 = drop_glue___env_1336;
...
__t160->__fn = (tur_thunk_int64_t_int64_t_t)__fn_1334;
```

and slot 0 genuinely holds the code pointer. The inline variant works
because it takes a different path entirely -- the argument is wrapped as
`(tur_poly_fn_t){ NULL, (int64_t(*)(void*,int64_t))__poly_1334 }`, a
`__poly_` adapter with the env parameter spliced in, which never consults
slot 0.

So there are three lowerings of "a `:fn` value" and the let-bound
non-capturing one produces a representation the consumer does not accept.

## Fix directions

Not root-caused to a single line. Two shapes of fix, in rough order of
preference:

1. Make the let-binding of a non-capturing lambda produce the same
   representation the other two produce -- either a real one-slot box, or
   route it through the `__poly_` adapter the inline path already builds.
   That keeps exactly one consumer contract.
2. Failing that, make the argument-materialization site distinguish the two
   cases. That means the carrier has to be self-describing, which it is not
   today -- which is the argument for (1).

The relevant sites are the `:fn` let-binding lowering and the
carrier-to-`tur_poly_fn_t` materialization in `src/compiler/emit_expr.c`
(search `tur_poly_fn_t){`).

## Note for whoever picks this up

Worth checking whether the same three-way split affects storing a
non-capturing lambda in a struct field, a vector, or a global -- the repro
only exercises a `let`, but nothing about the diagnosis is specific to
`let`.
