# An instance method called from inside another instance body of the same class is specialized with the ENCLOSING instance's result type

**Severity: medium-high** -- a hard `cc` rejection of a well-typed program,
nonlocal by construction (the two instances need not share a file, a module, or
a spice), and it is latent until one instance of the class has a by-value
result. Found while sweeping the msgpack spice's workarounds after the four
reports of 2026-09-14 were fixed; it is what kept the fourth one's workaround
in place.

**Status: RESOLVED 2026-09-15.** Fixed in `emit_abi_register_call`
(`src/compiler/emit_module.c`), pinned by
`tests/fixtures/instance-method-forwards-inside-instance-body`.

## Repro

Twenty lines, no msgpack involved. Two things are load-bearing; drop either and
it compiles with the bug present.

```turmeric
(defmodule repro (export)

(defclass D [a] (dec [n : int] : (Result a cstr)))

;; (2) The primitive instance FORWARDS to a separately-declared typed function
;;     rather than building the Result inline.
(defn get-int [n : int] : (Result int cstr)
  (:: (ok n) (Result int cstr)))

(definstance D [int] (dec [n] (get-int n)))

;; (1) A VALUE-STRUCT instance of the same class -- what a derive-codec macro
;;     emits for a defstruct. This is what mints a by-value Result clone.
(defstruct Pt [x : int y : int])

(definstance D [Pt] (dec [n]
  (:: (ok (make-struct Pt
            (ok-val (:: (dec n) (Result int cstr)))
            (ok-val (:: (dec n) (Result int cstr)))))
      (Result Pt cstr))))

(defn main [] : int
  (let [p (ok-val (:: (dec 7) (Result Pt cstr)))]
    (- (+ (. p x) (. p y)) 14)))
)
```

```
error: passing 'tur_adt_Result__Pt__cstr' to parameter of incompatible type
       'tur_adt_Result__int__cstr'
error: returning 'tur_adt_Result__int__cstr' from a function with incompatible
       result type 'tur_adt_Result__Pt__cstr'
```

## Root cause

`emit_abi_register_call`'s spec-interning rehydration
(`src/compiler/emit_module.c`, the "(2) re-hydrate carrier-collapsed bindings
by name" arm) matches type bindings **by name** against the active
specialization's bindings. A class's type parameter carries ONE name across
every instance of that class, so inside `D [Pt]`'s body -- where the active
spec binds `a -> Pt` -- the callee `__inst_D_dec_int`'s own `a` was rehydrated
to `Pt`. The `constrained-defn-monomorphize` block downstream then instantiated
the callee's declared result `(Result a cstr)` through that binding and
produced `(Result Pt cstr)`, so the call interned

```
__inst_D_dec_int__spec__tur_adt_Result__Pt__cstr_int64_t
```

whose body forwards `get-int`'s `Result int cstr`.

The callee's `a` is not free: the instance pins it, and `D [int]`'s `a` IS
`int` by declaration. This is the tyvar-name capture of
[generic-unwrap-specializes-by-the-enclosing-type-argument](generic-unwrap-specializes-by-the-enclosing-type-argument.md)
one layer up -- there the captured name was a `defn`'s type parameter, here it
is a class's.

Two observations narrow it:

- The **correct** spec is interned as well. `--emit-abi-trace` shows the ABI
  pass registering `__inst_D_dec_int__spec__tur_adt_Result__int__cstr_int64_t`,
  and it is emitted -- just never called. The bad clone is minted later, during
  the re-scan of the `D [Pt]` spec body, with
  `current_abi_specialization = __inst_D_dec_Pt__spec__...`.
- The sibling `ok-val` on the same source line keys correctly
  (`ok_val__spec__int64_t_tur_adt_Result__int__cstr`). It is the callee's
  result type that is captured, not the whole expression, which is why an
  explicit `(:: (get-int n) (Result int cstr))` on the forward does not help.

## Why it stayed hidden

While every instance of the class is carrier-shaped, both spellings c-name to
`int64_t` and the wrong name is only a wrong name. One by-value instance result
is what turns it into a type error. That is also why the existing
`tests/fixtures/struct-instance-byvalue-result-consumer` fixture does not catch
it: its primitive instance builds the Result **inline** with `ok`, so the body
is specialized to the same wrong result type and stays internally consistent.
The forward is what makes the mismatch observable.

## Fix

In the by-name rehydration, when the callee is itself an instance method, take
the pin for a class-type-parameter name from the callee's own
`owner_instance->type_args` instead of searching the active spec. Deliberately
does not set `any`: the clause only ever WITHHOLDS a substitution that was
wrong, so on its own it leaves the binding carrier-collapsed exactly as it was
before the active spec existed.

## What this unblocks

`turmeric-spices/spices/msgpack`'s four primitive `DecodeMp` instances, which
had to stay hand-written carrier-shaped inline C. They are one-line forwards
again:

```turmeric
(definstance DecodeMp [int]   (decode-mp [tree node] (mp-get-int   tree node)))
(definstance DecodeMp [cstr]  (decode-mp [tree node] (mp-get-str   tree node)))
(definstance DecodeMp [bool]  (decode-mp [tree node] (mp-get-bool  tree node)))
(definstance DecodeMp [float] (decode-mp [tree node] (mp-get-float tree node)))
```

That was the last row of
[msgpack-spice-workarounds-to-remove](msgpack-spice-workarounds-to-remove.md).
json is exposed to the identical shape and compiles only because its primitive
instances are already carrier-shaped inline C.
