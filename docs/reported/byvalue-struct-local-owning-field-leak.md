# Codegen: a by-value struct/ADT local with an owning field leaks it at scope exit

**Severity:** medium (memory leak; no crash / miscompile). Mainline codegen --
not CPS-backend-specific. Likely part of the deferred owning-pointer lifecycle
work (graduation-gate "item 4"), but not currently tracked anywhere.

## Summary

A local binding of a by-value struct / record-ADT that has drop glue (an owning
`rc` / `ref` / `weak` field, i.e. `AdtDef.needs_drop_glue == true`) is never
released at scope exit. A *bare* `rc` local IS auto-dropped (a `(defer (drop!
r))` is injected, lowering to `rc_strong_decrement`), but an `rc` nested inside a
by-value struct local is not -- the generated `drop_glue_tur_adt_<T>` is emitted
but never called, so the field's control block and payload leak.

## Minimal repro

```turmeric
(defstruct Own [r : rc<int> tag : int])
(defn f [] : int
  (let [o (make-struct Own :r (rc/of 7) :tag 9)]   ; o owns an rc<int>
    (.tag o)))                                      ; o does not escape
(defn main [] : int (println (f)) 0)
```

`tur emit-c` for `f`:

```c
static int64_t f() {
    int64_t __t181;
    {
        int64_t *__t182 = (int64_t *)malloc(sizeof(int64_t));
        *__t182 = INT64_C(7);
        RcControlBlock *__t183 = rc_cb_alloc(0, 3, NULL);   // refcount = 1
        __t183->value = __t182;
        tur_adt_Own o_1267 = ctor_Own(__t183, INT64_C(9));
        __t181 = (int64_t)(o_1267).tag;
    }                                                       // <-- no release of o's rc
    return __t181;                                          // __t183 + __t182 leak
}
```

`drop_glue_tur_adt_Own` is *defined* in the output but never *called*. Contrast a
bare `rc` local, which gets `struct __defer_env {...}` + `rc_strong_decrement` at
scope exit.

## Root cause

`src/compiler/elab_forms.c` (elab_let, the scope-exit auto-drop injection near
lines 960-1000) counts and injects `(defer (drop! b))` only for bindings whose
`type.kind == TY_REF` (linear refs). There is no arm for a by-value ADT local
whose `AdtDef.needs_drop_glue` is set, so such a local's fields are never
released. The drop glue itself is generated (`emit_adt_byval_drop_glue`,
`src/compiler/emit_module.c`), so only the *invocation* at scope exit is missing.

(The bare-`rc` auto-drop is injected through a separate path, which is why an
`rc` local is released but the same `rc` nested in a by-value struct local is
not.)

## Fix directions

Extend the elab_let auto-drop injection to also fire for a local binding whose
type is a by-value ADT (`TY_ADT` / `TY_APP`) with `needs_drop_glue`, mirroring
the existing `TY_REF` count + inject loops: inject a scope-exit
`drop_glue_tur_adt_<T>(&local)` (or the equivalent `(defer (drop! local))` that
lowers to it), guarded by the same moved / explicitly-consumed / consumed-by-use
filters so it does not double-drop a value that escapes or is passed to a
consuming call.

## Relationship to the CPS backend

The `^borrow` owning-capture slice
(`docs/reported/cps-backend-owning-capture-multishot-double-free.md`, partially
resolved) relies on a `^borrow` value being read-only and never dropped by the
callee -- which is a type-system guarantee independent of this leak. This leak is
orthogonal: it is about an *owned* local (not a borrow) failing to be released,
in the ordinary direct emitter.
