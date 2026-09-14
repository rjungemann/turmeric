# A value-struct instance flips a class's Result representation, and a generic `ok-val` is not told

**RESOLVED 2026-09-14**, and it corrects the report on where the defect lives.

Everything the report establishes about the TRIGGER holds -- it is the
value-struct instance, the controls bracket it correctly, and json's escape is
the accident the report names. What is wrong is the root cause. This is **not**
a producer/consumer specialization-selection split in `emit_module.c`'s ABI
pass: that pass gets it right. Instrumenting it shows the `ok-val` call inside
`build`'s specialization interning exactly the spec it should,
`ok_val__spec__int64_t_tur_adt_Result__int__cstr`, and the emitted
`build__spec__...` body calls it.

The broken body is the **carrier base** `build` -- which the emitted C carries
alongside the spec -- and there the mismatch runs the other way: it is the
PRODUCER that should have stayed on the carrier and did not. With no per-`Expr*`
recording for a call in a generic body, `find_matched_abi_spec` (`emit_expr.c`)
falls back to a structural match on the callee binding and the argument types --
and a return-type-dispatched class method's by-value clone and its carrier base
have **identical arguments**, differing only in the return. The one guard
against that is `emit_spec_result_mismatch`, which declines to narrow when
either side's result is "not decisive". A return-dispatched `(dec ...)` in a
generic body has a bare `TY_TYVAR` result, which counted as not decisive, so the
by-value `__inst_Dec_dec_int__spec__...` matched. Its
`tur_adt_Result__int__cstr` was then handed to the carrier `ok_hyval(int64_t)`,
which is the cc error in the report. (Two call sites reach that comparison; the
other one's result had already collapsed to `TY_INT` and the existing C-name
compare rejected the spec correctly. Only the tyvar-result one slipped through.)

The fix is to recognize that an abstract result is not "cannot tell": a value
whose type still mentions a named tyvar is being emitted in a generic body,
where every carrier-ABI value rides the int64 carrier. `emit_spec_result_mismatch`
now treats that as decisive against a spec whose result is a concrete by-value
aggregate. It is a pure narrowing -- it can only turn a wrong hit into "no
spec", which is the erased carrier base a generic body wants.

So fix directions 1 and 2 are both moot; nothing had to follow its producer, and
no bridge was needed. The by-value instance clone simply must not be visible to
a carrier body, which is what the `active_outer == NULL` guard a few lines above
already says for the recorded case.

Direction 3's fixture is in:
`tests/fixtures/struct-instance-byvalue-result-consumer`, with the value-struct
instance load-bearing (delete it and the program compiles even with the bug
present).

## Original report

**Severity: medium-high** -- a **hard cc failure**, not a silent wrong answer,
but the trigger is nonlocal and counterintuitive: adding an instance for one
type breaks a generic that never mentions that type.

**Status:** open. Found 2026-09-14 building `spices/msgpack` (the MessagePack
spice from [msgpack-spice-plan](../upcoming/hold/msgpack-spice-plan.md)), whose
`DecodeMp` class and `decode-mp-list` mirror the json spice's `DecodeJson` /
`decode-json-list` one for one.

## Repro

A return-type-dispatched class, two instances, and a constrained generic that
takes `ok-val` of the dispatched result:

```turmeric
(defopaque Tree :ptr<void>)
(defopaque Node :int)

(defstruct Pt [x : int  y : int])

(defclass Dec [a] (dec [t : Tree  nd : Node] : (Result a cstr)))

(definstance Dec [int] (dec [t nd] (:: (ok (:: nd :int)) (Result int cstr))))

;; The second instance is at a VALUE-STRUCT type -- exactly what `derive-json`
;; and `derive-msgpack` emit for a defstruct.
(definstance Dec [Pt] (dec [t nd]
  (:: (ok (make-struct Pt (ok-val (:: (dec t nd) (Result int cstr)))
                          (ok-val (:: (dec t nd) (Result int cstr)))))
      (Result Pt cstr))))

(defn mk-node [i : int] : Node (:: i Node))

(defn build [A] [(Dec A)] [t : Tree  i : int  n : int] : (Cons A)
  (if (>= i n)
    (:: (tnil) (Cons A))
    (tcons-of (ok-val (:: (dec t (mk-node i)) (Result A cstr)))
              (:: (build t (+ i 1) n) :int))))

(defn null-tree [] : Tree
  ```c
  return NULL;
  ```)

(defn main [] : int
  (let [xs (:: (build (null-tree) 0 3) (Cons int))]
    (println (.head xs))
    0))
```

```
$ tur run p.tur
p_tur.c:8167:42: error: passing 'tur_adt_Result__int__cstr'
    (aka 'struct tur_adt_Result__int__cstr') to parameter of
    incompatible type 'int64_t' (aka 'long long')
1 error generated.
tur: cc invocation failed (status 256)
```

The emitted body of `build` at `A = int`:

```c
tur_adt_Result__int__cstr __ps_215 =
    (__inst_Dec_dec_int__spec__tur_adt_Result__int__cstr_void___int64_t(...));
int64_t __ps_216 = (ok_hyval(__ps_215));   /* <-- carrier ok-val, struct arg */
```

The instance is emitted as a `__spec__` clone returning the **by-value**
`(Result int cstr)` struct, while the `ok-val` at the call site is still the
unspecialized carrier `ok_hyval(int64_t)`.

## Controls -- what is and is not the trigger

Three variants of the repro above, each changing exactly one thing:

| Variant | Result |
| --- | --- |
| Delete the `Dec [Pt]` instance | **compiles and runs** |
| Second instance at a SCALAR type (`Dec [cstr]`) instead of the struct | **compiles and runs** |
| `Dec [int]`'s body forwards a named `(Result int cstr)`-returning defn instead of building `ok` inline | still fails, identically |

So it is **not** about how the `int` instance's body is written -- that was the
first guess and it is wrong. The trigger is the presence of a **value-struct**
instance of the same class. Adding one flips the class's method onto the
by-value Result representation program-wide; the `ok-val` inside the generic
is not re-specialized to match.

Note the shape this gives a user: the generic and the struct need not be in the
same file, the same module, or even the same spice. Deriving a codec for one
more struct is what breaks a list decoder that was working.

## Why json has never hit this

`spices/json` has the identical shape -- `Decode`/`DecodeJson`, a
`derive-json`-emitted value-struct instance, and `decode-json-list`'s
constrained generic taking `ok-val`. It compiles only because its four
primitive instances are inline C returning `tur_box_ok` / `tur_box_err`, i.e.
already carrier-shaped, so the mismatch cannot arise. That is an accident of
how they were written, not a property of the design: rewriting any one of them
as a Turmeric-level body would break `decode-json-list` the same way.

## Root cause

The ABI-specialization pass in `src/compiler/emit_module.c` picks a
`(result_type, arg_types)` per call and interns a clone named by
`emit_abi_clone_name` (`emit_module.c:3140`), via `emit_abi_intern_spec`
(`:3164`). The instance method gets a spec whose result is the by-value
`Result` struct, but the `ok-val` applied to that call's value keeps the
carrier signature -- the two are chosen independently, with nothing forcing the
consumer's specialization to follow its producer's.

`emit_module.c:3550`'s comment on `ok__spec__Result__int__int` shows the same
family of concern on the construction side; this is the destructuring side of
it.

## Fix directions

1. When a call's specialized result type is a by-value aggregate, specialize
   its *consumers* (`ok-val`, `err-val`, `ok?`, `err?`, `unwrap`, `some?`) to
   the same representation, rather than leaving them at the carrier. The
   specialization already exists -- `ok_val__spec__double_tur_adt_Result__float__cstr`
   is emitted elsewhere in the same program -- so this is a selection bug, not
   a missing capability.
2. Failing that, insert the carrier bridge on the *consumer* side, the way the
   non-specialized instance already gets one on the producer side (the
   `malloc` + store + `(int64_t)(intptr_t)` box visible in
   `__inst_Dec_dec_int` when no struct instance exists).
3. Either way this deserves a fixture: two instances of one return-dispatched
   class, one of them a value-struct, consumed by a constrained generic.

A diagnostic would be a poor substitute here -- there is nothing the author of
either the generic or the struct did wrong.

## Workaround in use, and what to remove when this is fixed

`spices/msgpack/src/msgpack/encode.tur` writes its four primitive `DecodeMp`
instances as inline C that builds the Result with `tur_box_ok` / `tur_box_err`,
purely to keep every instance carrier-shaped. They would otherwise be one-line
forwards to the typed reads that `msgpack/decode` already exports:

```turmeric
(definstance DecodeMp [int]   (decode-mp [tree node] (mp-get-int   tree node)))
(definstance DecodeMp [cstr]  (decode-mp [tree node] (mp-get-str   tree node)))
(definstance DecodeMp [bool]  (decode-mp [tree node] (mp-get-bool  tree node)))
(definstance DecodeMp [float] (decode-mp [tree node] (mp-get-float tree node)))
```

**When this report is resolved, restore exactly those four lines** and delete
the ~20-line comment block above them that explains the boxing, along with the
duplicated error-message strings the inline C carries. The
`spices/msgpack/tests/container-round-trip.tur` cases already cover the
`decode-mp-list` path at `int`, `cstr` and `float`, so the revert is verified
by running that suite -- a regression re-breaks the build rather than passing
quietly.
