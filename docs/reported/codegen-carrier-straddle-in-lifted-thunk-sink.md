# A lifted thunk's result sink takes the int64 carrier into a typed-pointer temp

**Severity: medium (hard `cc` error on clang / GCC >= 14, so `tur build` of an
affected program fails outright; a warning and layout-lucky success on older
GCC).** Filed 2026-09-07.

**Found by `tests/regions-fuzz-src.py`** (seed 1, program 0) -- the regions
fuzzer generating a shape no fixture had. It has nothing to do with regions:
it reproduces with `TUR_REGIONS=0`, with no region form in the program, and on
`origin/main` itself.

## The straddle

A lifted closure thunk whose body is a `do` ending in a call that returns the
**int64 carrier** gets a result sink declared with the **concrete pointer**
type, and the assignment between them is unbridged:

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

Note the return IS bridged; only the sink assignment is not. `ctor_TL_TNil()`
hands back `tur_adt_TL *` while `tbuild` returns the carrier, so the same type
has both representations in one function and the sink picked the wrong one.

## Repro

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
item** (the `chain-sum` call) ahead of the tail, and the tail must be a call
returning the carrier for a `:heap` ADT. Drop either and the straddle goes away.

## Not a regression, and not clang-only

- **On `origin/main`.** Built e89594fb in a clean worktree from main's own
  sources: same error, same site. It is not from the regions branch that found
  it.
- **GCC warns at the identical site** -- `warning: assignment to 'tur_adt_TL *'
  from 'int64_t' ... [-Wint-conversion]` -- so this is not a clang quirk. It is
  a hard error only where the compiler promotes it (clang, GCC >= 14), which is
  why macOS CI goes red and Linux does not.
- `tests/run.sh`'s own emitted-C pointer/integer ratchet greps each fixture's
  build stderr for exactly this warning and fails the fixture on it. It stays
  quiet only because **no fixture generates this shape** -- which is the whole
  reason a fuzzer found it and the tree did not.

This also refutes, narrowly, the claim in `src/main.c` (the comment where the
two `-Wno-error=` downgrades were removed) that "the whole fixture tree emits 0
-Wint-conversion / -Wincompatible-pointer-types hard errors". True of the
fixture tree; not true of the language.

## Fix direction

The sink assignment is the stackless/CPS result-sink lowering
(`src/compiler/emit_fns.c`, the `sink->dest` write around line 1890, and the
sibling `gs->vars[...].cname` writes). Bridge there the way the return already
does: when the sink's declared C type is a concrete pointer and the value is
the int64 carrier, run it through `emit_carrier_bridge(..., CK_CARRIER,
CK_CONCRETE, t)` instead of assigning raw. The alternative -- declare the sink
as the carrier and let the existing return bridge do the work -- is probably
smaller but changes the temp's type everywhere it is read.

Either way it wants its own change with the snapshots regenerated: the sink
lowering is shared by every stackless-lowered thunk, so this is not a local
edit. Pin the repro above as a fixture in the same change, since the ratchet
will then keep it honest on Linux too.
