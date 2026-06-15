---
title: Return-type dispatch on a constrained type variable silently mis-resolves to the `ptr<void>` instance
category: Typeclass dispatch / elaboration -- silent miscompile
severity: Medium-high. SILENT miscompile, not a hard error. A typeclass method
  with a return-type-only class variable (e.g. `Serializable`'s `deserialize [b
  : ptr<void>] : a`), invoked inside a polymorphic function under a `(Class A)`
  constraint and ascribed to the constraint var (`(:: (deserialize b) A)`),
  does NOT dispatch to A's instance. It silently picks the `ptr<void>` instance
  and emits a wrong-instance call. No type error is raised; the program compiles
  and then misbehaves at runtime. This is exactly the class of bug CLAUDE.md
  flags ("works by luck because the register classes happen to match" / silent
  miscompile).
status: OPEN -- 2026-06-15. Discovered while scoping the M4 bucket-D fix
  (docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md
  "Resolution 2026-06-15"). Not fixed this session; the bucket-D fix routed
  around it (format-preserving Pair serialize that needs no recursive
  deserialize). Filed so it is not forgotten.
---

# Return-type dispatch on a constrained type var silently picks the `ptr<void>` instance

## One-line summary

`(:: (deserialize b) A)` with `A` a `(Serializable A)`-constrained type
variable resolves `deserialize` to `__inst_Serializable_deserialize_ptr_void`
instead of A's instance -- a silent wrong-instance dispatch, no diagnostic.

## Minimal repro

```turmeric
(load "stdlib/serial.tur")
(extern-c printf [^cstr fmt ^int v] :int)

;; round-trips x through serialize/deserialize, dispatching deserialize to A's
;; instance via a return-type ascription to the constraint variable A.
(defn round [A] [(Serializable A)] [x : A] : int
  (let [b (serialize x)]
    (:: (deserialize b) A)))

(defn main [] : int
  (printf "got=%lld\n" (round 42))   ; expect got=42
  0)
```

Build and run:

```
$ ./build/tur build /tmp/probe.tur -o /tmp/probe && /tmp/probe
deserialize bytes: buffer too short for payload     # <- WRONG: ran ptr<void> deserialize
$ echo $?
1
```

## Observed vs expected

- **Expected:** `(:: (deserialize b) A)` with `A = int` (the call-site monomorph
  of `round`) dispatches to `__inst_Serializable_deserialize_int`, recovering
  `42`. (Or, if return-dispatch on a type var is genuinely unsupported, a
  **compile-time error** -- `TUR-E00xx: cannot dispatch return-type-polymorphic
  method 'deserialize' on type variable 'A'`.)
- **Observed:** the emitted spec body for `round` is

  ```c
  static int64_t round(int64_t x) {
      int64_t __t23;
      {
          void * b_972 = __inst_Serializable_serialize_int(x);     /* serialize: OK, dispatched to int */
          __t23 = __inst_Serializable_deserialize_ptr_void((void *)(intptr_t)(b_972));  /* WRONG */
      }
      return __t23;
  }
  ```

  `serialize` correctly dispatched to the `int` instance (it has a value arg of
  type `A` to dispatch on). `deserialize` -- which has **no argument of type
  `A`**, only the return -- fell back to the `ptr<void>` instance and ran its
  body, which then hit the `buffer too short` guard at runtime. No type error,
  no warning.

## Root-cause analysis (preliminary)

`deserialize`'s signature is `(deserialize [b : ptr<void>] : a)` -- the class
variable `a` appears **only in the return type**. Dispatch normally keys off an
argument whose type carries the class variable; here there is none, so the
selector has nothing to match and resolves against `ptr<void>` (the literal
type of the only argument `b`), picking `__inst_Serializable_deserialize_ptr_void`.

The ascription `(:: (deserialize b) A)` is intended to drive **return-type
dispatch** (use the ascribed result type `A` to select the instance). M5 added
return-side `abi_bindings` for ascribed dispatch
(`src/compiler/elab_typeclasses.c:3191` -- "M4c Path A return-side"), but that
path appears to (a) only fire for a *concrete* ascribed type, not a type
variable `A` still abstract inside a constrained polymorphic body, and/or (b)
not consult the `(Serializable A)` constraint dictionary to pick the instance.
When `A` is abstract, the selector silently falls through to the argument-type
(`ptr<void>`) match instead of erroring.

Pointers to confirm:
- `src/compiler/elab_typeclasses.c` -- the method-dispatch instance search;
  find where a method with the class var only in the return type is resolved,
  and where the ascription's target type is (or isn't) fed into the selector.
- The return-side `abi_bindings` population (~`elab_typeclasses.c:3191`) -- does
  it handle a TYVAR ascription target by binding to the constraint dict, or only
  a concrete type?
- The fall-through that lands on `ptr<void>`: this is the silent path. At
  minimum it should be a hard error when the resolved type is a bare type var.

## Proposed fix directions

1. **Make it a hard error first (cheap, stops the silent miscompile).** When a
   method's class variable is return-only and the dispatch target type is an
   unresolved TYVAR, raise a typecheck error instead of falling back to an
   argument-type instance. This converts a silent runtime misbehavior into a
   compile-time diagnostic -- strictly better even before real support lands.

2. **Support constrained return-dispatch via the constraint dictionary.** Inside
   a `(Serializable A)`-constrained fn, the dictionary for A is in scope; a
   return-type ascription to `A` should dispatch `deserialize` through that
   dict's `deserialize` slot. This is the same dict-passing the constrained-poly
   path already uses for argument-dispatched methods; extend it to the
   return-only case. This is the principled fix and would unblock recursive
   composite `deserialize` (e.g. a real `Serializable [Pair]` that recovers
   by-value-struct elements -- see the M3 report's bucket-D discussion).

## How to validate a fix

- The repro above prints `got=42` and exits 0.
- A negative test: `(:: (deserialize b) A)` with no `(Serializable A)`
  constraint in scope is a compile error (no dict to dispatch through).
- Full suite stays green; add a fixture pinning the round-trip
  (`tests/fixtures/serial-return-dispatch-tyvar/`).

## Related

- [docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md](m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)
  "Resolution 2026-06-15" -- bucket-D fix routed around this gap.
- `stdlib/serial.tur` -- the `Serializable` class; its `deserialize` is the
  canonical return-type-polymorphic method and carries a comment noting it
  "cannot dispatch on a type var" (this report is the concrete failure mode
  behind that comment, plus the observation that the failure is *silent*).
