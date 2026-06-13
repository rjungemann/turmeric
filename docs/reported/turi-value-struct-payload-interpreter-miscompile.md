# `ok`/`err` with a value-struct payload silently miscompiles under `--interpret`

**One-line summary:** Under `tur --interpret`, a `Result` carrying a *value
struct* payload (`(ok (make-struct User ...))`) loses the struct tag when read
back via `ok-val`/`err-val`, so a subsequent field access reads the recovered
carrier as a raw `int64` -- printing a garbage pointer instead of the field.

**Severity:** High -- **silent miscompile** (rc=0, wrong stdout). The compiled
path is correct (Prereq 6 synthesizes a by-value Result struct); only the
interpreter diverges, and it diverges *silently*. Two fixtures affected.

## Minimal repro

`tests/fixtures/polymorphic-ok-err-value-struct-payload/input.tur`:

```turmeric
(defstruct User [id : int  name : cstr])
(defstruct Errm [code : int  message : cstr])

(defn main [] : int
  (let [r-ok  : (Result User cstr) (ok  (make-struct User 7 "alice"))
        u                          (ok-val r-ok)
        r-err : (Result int Errm)  (err (make-struct Errm 42 "boom"))
        e                          (err-val r-err)]
    (println (.name u))
    (println (.message e)))
  0)
```

Observed vs expected:

```
$ tur --interpret input.tur          $ tur run input.tur  (and expected.stdout)
-4702111237675155454                 alice
-4702111237675155454                 boom
```

`typeclass-return-dispatch-result-wrapped` exhibits the same class: it prints
`42` then a garbage int where `hello` is expected (a return-dispatch class method
whose `Result` wraps a cstr payload).

## Root-cause analysis

The interpreter's `Result` natives (`native_ok` / `native_err` /
`native_ok_val` / `native_err_val`, `src/main.c`) carry the payload through the
`int64[3] {is_ok, ok, err}` box. When the payload is a `make-struct User`, that
payload is a `TuriStruct*` stored verbatim as `int64`. `ok-val` returns it as a
bare `turi_int(carrier)` -- the `TURI_STRUCT` tag is dropped -- so the
downstream `(.name u)` / `EX_GET_FIELD` sees a `TURI_INT` and reads the pointer
value as the field, then `println` formats the pointer as a signed int64.

This is the **same dual-rep gap** already fixed in two adjacent spots and should
reuse that machinery:

- the inline-C ADT-carrier re-tag (`src/turi/eval.c`, unified inline-C return
  point): when a function's declared return type is `TY_ADT`/`TY_STRUCT` and the
  executor produced a non-null `TURI_INT`, reinterpret the carrier as the
  original `TuriStruct*`;
- the `EX_GET_FIELD` carrier-box path (W1b, `src/turi/eval.c`): a struct that
  flowed via the int64 carrier reaches field access as a `TURI_INT` box and is
  read word-by-word, tagged by the field's static type.

The gap here is that **`ok-val`/`err-val` natives do not re-tag a struct
payload**: the value's static type at the binding (`u : User`) is known, so the
native (or the field-access site) should recover the `TuriStruct*` rather than
hand back a bare int. See the analogous `option_field` / `result_field`
dual-rep readers that already do this for the accessor side.

## Proposed fix directions

1. **Preferred:** when `ok-val`/`err-val`'s statically-known payload type is a
   struct/ADT and the carrier is a non-null `TURI_INT`, return
   `turi_struct_val((TuriStruct*)carrier)` -- mirroring the inline-C return
   re-tag. Non-null guard avoids a nil-carrier deref.
2. Alternatively, make `EX_GET_FIELD` on a `TURI_INT` whose binding static type
   is a `defstruct` reinterpret the carrier as `TuriStruct*` (broader; risks
   masking genuine non-struct carriers -- prefer (1)).

## Validation

- `polymorphic-ok-err-value-struct-payload` and
  `typeclass-return-dispatch-result-wrapped` print `alice`/`boom` and
  `42`/`hello` under `--interpret`, matching `expected.stdout`; add both to the
  `tests/run-turi.sh` allowlist.
- Re-run the full denylist probe; no regression in the allowlisted Result
  fixtures (`typed/result-basic`, `result-of-typed-eq`, ...).
- `tests/run.sh` unchanged (interpreter-only fix; no codegen touched).

Tracked for the flip in
[docs/upcoming/turi-interpret-flip-residual-plan.md](../upcoming/turi-interpret-flip-residual-plan.md)
(Bucket R5).
