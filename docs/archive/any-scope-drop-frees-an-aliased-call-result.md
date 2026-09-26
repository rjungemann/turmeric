# A let-bound `any` from a call is dropped even when the call returned an alias

**RESOLVED 2026-09-26, the day it was found** (while reading
`let_binding_any_freeable` for
[dynamic-returned-closure-env-is-never-freed](../reported/dynamic-returned-closure-env-is-never-freed.md)).
Filed here for the paper trail; it never had an open report.

**Severity at discovery: high** -- a use-after-free with a silent wrong answer
(exit 0) on the default build, in a common shape.

## Repro

```turmeric
(defstruct P [x : int y : int])
(defn show [a : any] : int (.x (cast a P)))
(defn main [] : int
  (let [xs (vec-new)]
    (vec-push! xs (:: (P 7 8) any))
    (let [e (vec-get xs 0)]
      (println (show e)))                 ; 7
    (let [b (P 9 10)] (println (.y b)))   ; reuses the freed box
    (println (show (vec-get xs 0))))      ; 23058839472, expected 7
  0)
```

The same happens for a call that returns a global `any`
(`(defn fetch [] : any g)`).

## Cause

The scope-exit drop of a let-bound `any` (`let_binding_any_freeable`,
`src/compiler/emit_expr.c`) counted the binding as owned when its initializer
was a widen performed there **or any call**. A call owns its result only when
the callee minted it: `vec-get` over a `(Vec any)` hands back the element the
vector still holds, and `fetch` hands back the global's box. The drop at the
inner `let`'s end freed a box its other holder kept, the next allocation reused
it, and the later read printed that allocation's bytes.

The other two `any` drop rules already asked the right question --
`any_expr_is_owned_temp` (`src/compiler/elab_call.c`): a widen, a call to a
`returns_fresh_any` producer, or a passthrough of an owned argument. The
move-to-use rule (`elab_forms.c`) and the argument drop (`elab_call.c`) both use
it; the scope-exit rule was the one that did not.

## Fix

`let_binding_any_freeable` admits a call initializer only when
`any_expr_is_owned_temp(init, 8)` holds. An owned result is still dropped at
scope end.

Pinned by `tests/fixtures/any-let-drop-aliased-call-result` (the global alias
and the `(Vec any)` element read, each read again after an inner scope closes)
and `tests/fixtures/any-let-drop-owned-call-result`, which carries
`requires.leak-check`: a fresh `mint` result and a passthrough of one are still
freed, and the global's alias is not.
