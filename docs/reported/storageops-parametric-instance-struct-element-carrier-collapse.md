---
title: A parametric StorageOps `(Dense A)` instance carrier-collapses a struct element type, blocking `defcomponent-accessors` from routing through `StorageOps`
category: Typeclass dispatch / associated-type value-level ABI -- expressiveness hole (and a silent miscompile for wide structs)
severity: Medium-High. A monomorphic helper that drives a `(Dense Pos)`
  handle through the *parametric* `(definstance StorageOps [(Dense A)] (type
  Elem = A) ...)` instance type-checks for the dispatch but fails value-level
  projection: `(.field (storage-get s i))` reports "no typeclass method found
  for '<field>'", and even with an explicit `: Pos` ascription the emitted impl
  rides the int64 carrier (`storage-get` returns `int64_t`, `storage-insert!`
  takes `int64_t v`). For a 1-word struct (`Pos [x:int]`) this *accidentally*
  round-trips because the layout is int64-compatible; for any wider struct the
  carrier read/writes only 8 bytes and silently corrupts. This is the open
  follow-up the ECS plan gates "routing `defcomponent-accessors` through
  `StorageOps`" against.
status: OPEN
---

# Parametric `StorageOps [(Dense A)]` carrier-collapses a struct `Elem`

## One-line summary

The earlier value-level-projection fix
(`docs/archive/typeclass-struct-result-rides-carrier-blocks-value-projection.md`)
threads a by-value struct result *only* when the instance binds the associated
member to a **non-parametric concrete** type (`(definstance StorageOps [(Dense
Pos)] (type Elem = Pos))`). The ECS spice ships the **parametric** instance
`(definstance StorageOps [(Dense A)] (type Elem = A))`, where `Elem`
substitutes to the instance's own head tyvar `A`, not to a concrete struct.
That tyvar is erased to the int64 carrier in the emitted impl, so neither the
call-site result type nor the impl C signature ever sees `Pos`.

## Minimal repro

```turmeric
(extern-c printf [^cstr fmt ^int v] : int)

(defopaque Dense [A] :int)
(defstruct Pos [x : int])

(defn dense-get [A] [^borrow s : (Dense A) idx : int] : A
  ```c return ((__TUR_TY_A__*)(intptr_t)s)[idx]; ```)
(defn dense-set! [A] [^borrow s : (Dense A) idx : int val : A] : nil
  ```c ((__TUR_TY_A__*)(intptr_t)s)[idx] = val; return; ```)
(defn dense-new [] : (Dense Pos)
  ```c static Pos arr[8]; arr[2].x = 42; return (intptr_t)arr; ```)

(defclass StorageOps [S]
  (type Elem : Type)
  (storage-insert! [^borrow s : S idx : int v : Elem] : nil)
  (storage-get     [^borrow s : S idx : int] : Elem))

;; PARAMETRIC instance: Elem = A (the handle's phantom), NOT a concrete struct
(definstance StorageOps [(Dense A)]
  (type Elem = A)
  (storage-insert! [s idx v] (dense-set! s idx v))
  (storage-get     [s idx]   (dense-get  s idx)))

(defn main [] : int
  (let [s : (Dense Pos)  (dense-new)]
    (storage-insert! s 6 (storage-get s 2))
    (printf "elem.x=%lld\n" (.x (storage-get s 6)))   ; <-- here
    0))
```

```
$ ./build/tur check /tmp/sop.tur
sop.tur:27:29: error: no typeclass method found for 'x'
```

Compare: the same program with a *non-parametric* `(definstance StorageOps
[(Dense Pos)] (type Elem = Pos) ...)` instance type-checks and lowers (that is
exactly `tests/fixtures/typeclass-assoc-type-method-return/`). The difference
is solely parametric-`A` vs concrete-`Pos` in the instance head.

## Observed vs expected

- **Observed.** `storage-get`'s result is typed by the bare carrier kind (no
  struct def), so `(.x ...)` cannot resolve. With an explicit `p : Pos`
  ascription the program *emits*, but the impl is the single carrier clone:

  ```c
  static int64_t __inst_StorageOps_storage_hyget_Dense__ltstruct_gt(int64_t s, int64_t idx) {
      return dense_hyget(s, idx);            // dense_hyget : int64_t(int64_t,int64_t)
  }
  // dense_hyget body, __TUR_TY_A__ == int64_t:
  //   return ((int64_t*)(intptr_t)s)[idx];  // reads 8 bytes only
  ```

  For `Pos [x:int]` (8 bytes) this round-trips by luck; for `Pos [x:int y:int]`
  it reads/writes the wrong element and only half the struct.
- **Expected.** Dispatching the `(Dense A)` instance at receiver `(Dense Pos)`
  binds `A -> Pos`, so `Elem = Pos`. The impl monomorphizes to a `Pos`-ABI
  spec (`dense_hyget` with `__TUR_TY_A__ == Pos`), `storage-get`'s result is
  `Pos` by value, and `(.x (storage-get s 6))` resolves.

## Root cause (file:line)

`src/compiler/elab_typeclasses.c`:

1. Return-type substitution (`elab_definstance`, ~line 2808-2819): for the
   parametric instance, `inst->assoc_types[ak]` for `Elem` is the tyvar `A`
   (the head's own parameter), so `return_type.kind == TY_TYVAR`. None of the
   `result_full_type` branches at ~3351-3385 fire (they require a concrete
   non-parametric `TY_STRUCT`/`TY_ADT`), so the impl binding keeps a tyvar
   carrier return.

2. Call site (`elab_method_call`, ~line 5046-5072 result-type computation,
   and ~line 5288-5304 the non-HKT `abi_bindings` attachment): the M4c Path A
   binding records only the **class var** `S -> (Dense Pos)`. The impl
   method's param/return types are written in terms of the **instance head
   tyvar** `A` (`v : Elem == A`, result `Elem == A`), which is never bound. So
   in `emit_abi_register_call` (`src/compiler/emit_module.c` ~line 1369-1450)
   the substitution finds nothing to change, `abi_changes` stays false, and no
   by-value spec is minted -- the call rides the carrier clone
   `__inst_StorageOps_storage_hyget_Dense__ltstruct_gt`.

The instance method binding also has `arg_full_types == NULL` (noted at
elab_typeclasses.c:5192), so even the argument side has no parametric type for
the substitution to bite on.

## Fix directions

At the dispatch call site, derive the **instance head tyvar bindings** by
unifying `inst->type_args[recv_pos]` (`(Dense A)`) against the receiver's
concrete type `obj_orig_type` (`(Dense Pos)`) -> `A -> Pos`, and:

1. Attach those as `abi_bindings` (in addition to / instead of the bare
   `S -> (Dense Pos)` class-var binding) so `emit_abi_register_call` mints a
   `Pos`-ABI spec for the instance method (the `v : Elem` arg and the `Elem`
   result both instantiate to `Pos`, `abi_changes` true, and the inner
   `dense-get`/`dense-set!` calls compose `A -> Pos`).
2. Set the instance method binding's `arg_full_types` / `result_full_type`
   to the substituted-but-still-parametric types (referencing `A`/`Elem`) so
   the substitution at emit has parametric types to instantiate.
3. Project the call-site `result_type`: when the class method returns the
   associated member and the instance binds it to a head tyvar, substitute
   that tyvar through the receiver bindings; when the result is a
   non-parametric struct/ADT, commit it (with def) so `(.field ...)` resolves
   -- the same gate the concrete-instance fix already uses.

Track A's end-to-end monomorphization (`current_abi_specialization`,
`emit_abi_intern_spec`) is the substrate; this is wiring the parametric
associated-type element through it.

## How to validate

- The repro compiles and prints `elem.x=...` with a **2-field** `Pos`
  (`[x:int y:int]`) round-tripping all fields, not just the first 8 bytes.
- `(.field (storage-get s i))` resolves against the parametric instance.
- Then route `defcomponent-accessors` (`../turmeric-spices/spices/ecs/src/ecs/world.tur`)
  through `StorageOps` for struct components and confirm the spice suite stays
  green.

## Related

- `docs/archive/typeclass-struct-result-rides-carrier-blocks-value-projection.md`
  (the concrete-instance half of this -- resolved; this is the parametric half
  it deliberately scoped out).
- `docs/upcoming/ecs-spice-plan.md` (E2d-P6; "Routing `defcomponent-accessors`
  through `StorageOps`" residual follow-up).
- `../turmeric-spices/spices/ecs/src/ecs/storage-ops.tur` (the shipped
  parametric instance + its "carrier note / element scope" caveat).
