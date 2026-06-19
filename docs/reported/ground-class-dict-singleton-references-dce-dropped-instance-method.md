---
title: Ground (kind-*) typeclass dict singleton is emitted for an instance whose method body was dead-code-eliminated, leaving `__inst_<Class>_<method>_<T>` undeclared in the consumer TU
category: Typeclass instance codegen / dict-singleton-vs-body liveness mismatch -- miscompile (fails cc)
severity: High. Blocks any program that imports a module defining a ground
  (kind-*) typeclass with more than one instance and does not directly call
  every instance's method from the consuming translation unit. The emitter
  writes a per-instance dict singleton (`dict_<Class>_<T>_singleton`) whose
  `.method = __inst_<Class>_<method>_<T>` initializer references the instance
  method symbol, but the instance-method *body* (and its forward declaration)
  is dropped by dead-code elimination when no DIRECT `__inst_*` call site marks
  it live. Result: the generated C references an undeclared symbol and `cc`
  rejects the TU. This is exactly the json spice's `Decode` failure
  (`'__inst_Decode_decode_int' undeclared here`), 15/15 in `spices/json/tests`.
status: OPEN
---

# Ground-class dict singleton outlives its DCE'd instance-method body

## One-line summary

For a ground (all type params `KIND_STAR`) typeclass, `emit_stmt.c`
(`EX_INSTANCE_DEF`) unconditionally emits a dict struct + singleton for **every**
in-scope instance, and the singleton's function-pointer slot references
`__inst_<Class>_<method>_<T>`. But the instance-method **body** is only emitted
when liveness analysis (`emit_instance_is_live` / `emit_abi_fn_skip_generic`,
`emit_module.c`) sees a *direct* `__inst_*` carrier call. When the only path to
the method is through the dict (indirect / generic / return-type dispatch), or
through a different instance, the body is DCE'd while its dict singleton is kept
-- so the singleton initializer names a symbol that is neither declared nor
defined in that TU, and `cc` errors with
`'__inst_<Class>_<method>_<T>' undeclared here (not in a function)`.

The HKT (kind != `*`) path already closes this gap: it skips the **dead
instance's dict in lockstep** with skipping its carrier base
(`emit_stmt.c:416-422` guarded by `g_m7_hkt_enabled && is_hkt`, paired with
`emit_module.c:2605-2626`). Ground classes are explicitly excluded
(`emit_stmt.c:414`: "ground (kind-*) instances like Eq are untouched"), so the
dict/body liveness can diverge for them.

## Minimal repro

Two modules in a project (`tur build <dir>`), a ground class `Decode [a]` with
two instances, consumer reaches only some of them:

```turmeric
;; src/codec.tur
(defmodule codec
  (export)
  (defclass Decode [a] (decode [doc : int] : (Result a cstr)))
  (definstance Decode [int]
    (decode [doc] ```c return tur_box_ok((int64_t)(doc * 10)); ```))
  (definstance Decode [bool]
    (decode [doc] ```c return tur_box_ok((int64_t)(doc == 1)); ```)))

;; src/app.tur
(defmodule app
  (export)
  (import codec)
  (defn main [] : int
    (let [r (:: (decode 5) (Result int cstr))]   ;; reaches int only
      (println (ok-val r)))
    0))
```

```
$ tur build <dir>
.../app_tur.c:3270:15: error: '__inst_Decode_decode_bool' undeclared here
 3270 |     .decode = __inst_Decode_decode_bool,
```

Inspecting the consumer TU: `__inst_Decode_decode_int` is forward-declared and
defined (int instance is live -- direct call from `main`), `dict_Decode_int_singleton`
is fine; but `dict_Decode_bool_singleton` is emitted with
`.decode = __inst_Decode_decode_bool` while that symbol's body/decl was DCE'd.

The json spice hits the `_int` spelling of the same bug because there `decode`
is reached only through generic/return-type dispatch (no direct `__inst_*` call
site), so **every** instance body is DCE'd while the dicts survive. A repro with
an indirect dispatcher (`(defn run [a] [doc : int] : (Result a cstr) (decode doc))`)
reproduces identically.

## Root cause (file:line)

- Dict struct + singleton emission: `src/compiler/emit_stmt.c:400-594`
  (`EX_INSTANCE_DEF`). The dead-instance skip at lines **416-422** is gated on
  `g_m7_hkt_enabled && tc->type_param_kinds` with an `is_hkt` check; a
  ground class (all `KIND_STAR`) never takes the skip, so the dict is always
  emitted. The singleton slot is written at lines **579-591**:
  `.<method> = <binding-name>` where the binding name is `__inst_<Class>_<method>_<T>`.
- Instance-method body liveness / DCE: `src/compiler/emit_module.c`
  `emit_instance_is_live` (**2553-2561**) only counts a *direct* carrier call
  (`emit_abi_has_carrier_call`) as a liveness source -- it never treats "is
  referenced by an emitted dict singleton" as live. `emit_abi_fn_skip_generic`
  (**2564-2629**) drops the carrier body when no carrier call is noted.
- The HKT lockstep that ground classes lack: `emit_stmt.c:405-422` paired with
  `emit_module.c:2605-2626`.

## Fix directions

The dict singleton and the instance-method body must agree on liveness for
ground classes the same way they already do for HKT. Two viable directions:

1. **Symmetric lockstep (preferred, mirrors HKT).** Make ground-class dict
   emission skip a dead instance, AND make body-liveness count a *dict
   reference* as live -- so a dict that IS emitted always has its body. Note
   the M4c dispatch for ground classes reads the per-instantiation dict
   singleton, so "referenced by an emitted dict" is a real liveness source that
   `emit_instance_is_live` currently ignores; adding it fixes the json case
   (dict-dispatched int instance) directly.

2. **Forward-declare + force-emit on dict emission.** Whenever a dict singleton
   slot references `__inst_<Class>_<method>_<T>`, ensure that instance method's
   body is emitted in the same TU (suppress its DCE). Simpler to reason about
   but reintroduces the dead-instance body bloat the HKT path was added to
   avoid; acceptable for ground classes since their bodies are usually small
   inline-C.

Whichever path, add a regression fixture: a multi-module project with a ground
class + >=2 instances where the consumer reaches the method only through dict /
generic dispatch (so all bodies would otherwise DCE). Regenerate affected
`tests/fixtures/*/expected.c` snapshots in the same change.

## Impact / scope

- json spice: 15/15 fail in `spices/json/tests` (Track C type-checks but is
  unrunnable on tip-of-main).
- Any downstream spice importing a ground typeclass with multiple instances and
  not directly calling each instance's method per consumer TU.
- Regression introduced on the by-value / M7 path: the HKT dead-instance
  elimination work added the lockstep for HKT classes but did not extend the
  same dict/body coupling to ground classes.
