# A word-returning function is accepted into a `(fn [...] nil)` slot and called through a `void` type

**Severity: low-medium.** An indirect call through the wrong function type:
undefined behaviour, a trap under `-fsanitize=function` and WASM's
`call_indirect`, harmless on x86-64 and arm64 only because a `void` caller
ignores the result register.  No wrong answer is known.  Filed 2026-10-03,
found typing `with-cancel-guard`'s closures (stdlib-int-stand-in-audit).

## Repro

```turmeric
(defn run-it [^fat body : (fn [] nil)] : nil (body))
(defn main [] : int
  (let [n 41]
    (run-it (fn [] (+ n 1))))
  0)
```

`tur check`: exit 0.  `CC=clang` with `-fsanitize=function
-fsanitize-trap=function`: SIGILL (exit 132) at `(body)` -- the lambda's thunk
is `int64_t (*)(void *)`, called as `void (*)(void *)`.

## Two causes, one shape

1. **The structural check treats `nil` and a word as one carrier.**
   `fn_type_structurally_compatible` (`types.c`) compares result kinds with
   `fn_slot_same_carrier`, which only separates a float from a word.  A `nil`
   result is `void` in C, which is neither.
2. **A `: void` / `: nil` annotation on a defn does not make it void.**
   `(defn noop [x : int] : void (let [_ x] 0))` is emitted
   `static int64_t noop(int64_t)` and typed `(fn [int] int)`: the body's value
   wins over the annotation.  So the strict rule below rejects a function the
   programmer declared `void`, which reads as a checker bug even though the
   function really does return a word.

## Measured fix, and why it is not landed

Refusing `ek == TY_NIL && ak != TY_NIL` (both non-wildcard) in the result arm
of `fn_type_structurally_compatible`: `bash tests/run.sh` 3508/0, and of
turmeric-spices' 777 files exactly one goes from clean to `TUR-E0001`:
`spices/tourist-ws/tests/route_test.tur`, whose `noop-handler` is the
`: void`-with-a-value shape of cause 2, passed to `ws-route!`'s
`(fn [WsConn] void)`.  Landing the rule alone would turn that test red from
this repo, which cannot patch it.

## Fix directions

1. Make a `: void` / `: nil` annotation discard the body's value (emit
   `void`, evaluate the body for effect), then add the rule.  The spice test
   then passes unchanged.
2. Or, at the boundary, wrap a word-returning function bound for a `nil` slot
   in a discarding adapter (the reverse of the `nil_result_word` shim), and
   leave the checker permissive.
3. Or land the rule and the one-line spice fix together (`noop-handler`'s body
   ending in `nil`).

## Fixed alongside (2026-10-03)

`fn_type_structurally_compatible` returned "compatible" for every NULLARY
function before reaching the result check -- a nullary fn has no `arg_kinds`
array, and the early `return 1` for "nothing to compare" ran first.  So
`(fn [] float)` satisfied `(fn [] int)` and the callee read the result from the
wrong register.  It now checks the result at arity 0 too: suite 3508/0, no
turmeric-spices file affected.  Pinned by
`tests/fixtures/errors/nullary-fn-arg-result-checked`.
