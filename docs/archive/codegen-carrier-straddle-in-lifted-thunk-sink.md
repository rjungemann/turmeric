# A `do` join takes the int64 carrier into a typed-pointer temp

**Severity: medium (hard `cc` error on clang / GCC >= 14, so `tur build` of an
affected program fails outright; a warning and layout-lucky success on older
GCC).** Filed 2026-09-07. **Resolved 2026-09-07** --
`src/compiler/emit_expr.c`, both joins in `emit_do_value`.

**Found by `tests/regions-fuzz-src.py`** (seed 1, program 0) -- the regions
fuzzer generating a shape no fixture had. It has nothing to do with regions:
it reproduces with `TUR_REGIONS=0`, with no region form in the program, and on
`origin/main` itself.

## The straddle

A `do` whose tail is a call returning the **int64 carrier** for a `:heap` ADT
gets a result temp declared with the **concrete pointer** type, and the
assignment between them was unbridged:

```c
static int64_t __fn_1511(void * __env_p_1514) {
        ...
        tur_adt_TL * __t68;                                   /* declared: pointer */
        tur_adt_TL * __ps_69 = (ctor_TL_TNil());
        int64_t __ps_70 = (tbuild(__env->n, (int64_t)(intptr_t)(__ps_69)));
        __t68 = __ps_70;                                      /* <-- int64_t -> pointer */
        return (int64_t)(intptr_t)__t68;                      /* bridged back here */
}
```

Note the return IS bridged; only the join assignment was not. `ctor_TL_TNil()`
hands back `tur_adt_TL *` while `tbuild` returns the carrier, so the same type
has both representations in one function and the temp picked the wrong one.

## Repro

Pinned as `tests/fixtures/do-tail-carrier-into-heap-ptr-temp`, which also
covers the two joins the original repro did not reach (a plain `defn` body and
the defers arm).

```turmeric
(defdata Link :heap (Link [v : int nxt : int]))
(defdata TL :heap (TNil) (TCons :int :TL))
(defn build [n : int acc : int] : int
  (if (<= n 0) acc (build (- n 1) (:: (Link n acc) :int))))
(defn chain-sum [c : int] : int
  ```c
  struct { int64_t v; int64_t nxt; } *p = (void *)(intptr_t)c;
  int64_t acc = 0; while (p) { acc += p->v; p = (void *)(intptr_t)p->nxt; } return acc;
  ```)
(defn tbuild [n : int acc : TL] : TL
  (if (<= n 0) acc (tbuild (- n 1) (TCons n acc))))
(defn tsum [t : TL] : int
  (match t (TNil) 0 (TCons v r) (+ v (tsum r))))
(defn thunk-call [^fat body : (fn [] TL)] : TL (body))
(defn go [n : int] : TL
  (thunk-call (fn [] (do (chain-sum (build n 0)) (tbuild n (TNil))))))
(defn main [] : int (println (tsum (go 3))) 0)
```

```sh
CC=clang TUR_REGIONS=0 ./build/tur run repro.tur
# error: incompatible integer to pointer conversion assigning to 'tur_adt_TL *'
#        from 'int64_t' [-Wint-conversion]
```

Both parts of the shape are load-bearing: the `do` needs a **discarded first
item** (the `chain-sum` call) ahead of the tail, or the `do` collapses and no
join temp exists; and the tail must be a **call** returning the carrier, since
a constructor call hands back the concrete pointer and the two representations
already agree. Drop either and the straddle goes away.

## Not a regression, and not clang-only

- **On `origin/main`.** Built e89594fb in a clean worktree from main's own
  sources: same error, same site. It is not from the regions branch that found
  it.
- **GCC warns at the identical site** -- `warning: assignment to 'tur_adt_TL *'
  from 'int64_t' ... [-Wint-conversion]` -- so this is not a clang quirk. It is
  a hard error only where the compiler promotes it (clang, GCC >= 14), which is
  why macOS CI went red and Linux did not.
- `tests/run.sh`'s own emitted-C pointer/integer ratchet greps each fixture's
  build stderr for exactly this warning and fails the fixture on it. It stayed
  quiet only because **no fixture generated this shape** -- which is the whole
  reason a fuzzer found it and the tree did not. With the fixture landed the
  ratchet now holds the line on Linux too.

This also refuted, narrowly, the claim in `src/main.c` (the comment where the
two `-Wno-error=` downgrades were removed) that "the whole fixture tree emits 0
-Wint-conversion / -Wincompatible-pointer-types hard errors". True of the
fixture tree; not true of the language -- until the fixture existed.

## The fix, and where the filing's fix direction was wrong

The filing pointed at the stackless/CPS result-sink lowering (`emit_fns.c`,
`sink->dest`) and predicted "its own change with the snapshots regenerated ...
this is not a local edit". Both halves were wrong, and the way they were wrong
is the reusable part:

The lifted thunk in the repro is a red herring -- it is what made the shape
*visible*, not what produced it. Tagging every `"%s = %s;\n"` emitter in
`emit_expr.c` with a distinct marker and rebuilding pinned the site in one
pass: the join in **`emit_do_value`**, not any sink lowering. Marking the
candidates and letting the compiler say which one fired beat reading the
lowering, which is how the first (wrong) direction was arrived at.

The bridge itself already existed. `bridge_control_result_int_ptr` was written
for exactly this straddle under `gcc14-int-conversion`, and the three **`let`**
joins pair it with `bridge_control_value_to_byvalue_temp`. The two **`do`**
joins called only the by-value bridge, which keys on
`fn_body_tail_byvalue_carrier_type` and so covers carrier -> by-value
aggregate but never carrier -> pointer. The fix is the missing second call at
each, passing the same `(type, tail)` pair the matching
`emit_control_result_temp_decl` used, so the declaration and the assignment
agree by construction:

```c
v = bridge_control_value_to_byvalue_temp(ctx, body, v, last);
v = bridge_control_result_int_ptr(ctx, v, last->type, last);
```

Nine lines including comments, and **zero snapshot drift** across all 148 --
the regen the filing called for was not needed. Suite 2826/0.

A first attempt added the analogous pointer branch to the CPS `letraw` binder
in `emit_cps_ir.c`, on the reasoning that its bridge is guarded by
`strchr(bct, '*') == NULL` and so has the same hole. It did not fix the repro
(wrong path), and it was the sole cause of drift in two snapshots, where it
emitted a redundant `(T *)(intptr_t)((T *)x)` over text some earlier site had
already cast. It was reverted rather than papered over with a
"text already opens with a cast" guard: a speculative codegen change with no
repro and no fixture is worse than the hole it guesses at. If that path does
have the same straddle, the fuzzer that found this one is now in the tree to
produce it.
