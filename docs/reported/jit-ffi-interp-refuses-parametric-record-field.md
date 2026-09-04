# call-ptr under --interpret refuses a record with a parametric-monomorph field

**Severity: low** (clean refusal, no wrong answers) -- but it is a
**compiled/interpreted divergence**: the identical program compiles and runs
on the AOT path and is refused under `--interpret`. The accompanying
diagnostic also misdescribes the cause in every case it can fire (see
"The diagnostic is inaccurate" below), which is the part most likely to cost
someone an hour.

Filed 2026-08-21, immediately after `jit-ffi` graduated
([docs/archive/jit-ffi-c2mir-plan.md](../archive/jit-ffi-c2mir-plan.md)).
This is the second of the two items that plan's "Still open" section carries;
the first, the aarch64 HFA gap, already has its own report
([mir-aarch64-fp-aggregate-abi](mir-aarch64-fp-aggregate-abi.md)). Graduation
archived the plan, so without this file that item has no live tracker -- an
archived plan is not somewhere a triage pass looks.

## Summary

A `defstruct` field whose type is a concrete application of a parametric
record -- `(BoxW int32)`, `(Option cstr)`, `(Pair2 cstr int)` -- is inlined
**by value** into the emitted C aggregate by codegen. The interpreter's F4
struct marshaller cannot render that field's layout, because doing so needs
per-application type substitution, so it refuses the whole `call-ptr` rather
than describe a struct the callee does not have. The refusal is deliberate
and correct as far as it goes: mis-describing it is exactly the miscall class
F4 exists to prevent, and is what the nested-field bug fixed in `40826ba13`
actually was.

## Repro

Verified against `eebb8a1cb` (v0.38.0) on arm64 macOS, with a
`-DTUR_JIT=ON` build.

```turmeric
(defstruct BoxW [a]
  (raw :int32))

(defstruct Outer [b : (BoxW int32) tag : int32])

(defn main [] : int
  (unsafe
    (let [h (dlopen "/usr/lib/libSystem.B.dylib")
          p (dlsym h "abs")]
      (println (call-ptr p [Outer -> :int32]
                         (make-struct Outer (make-struct BoxW (:: 1 :int32)) (:: 2 :int32))))))
  0)
```

```
$ tur emit-c repro.tur            # compiled path
rc=0                              # accepted; the field is inlined by value

$ tur --interpret repro.tur       # JIT build
tur: call-ptr: arg 0's record has a field with no by-value C member type
exit 1
```

Exit status and stream are correct (1, on stderr, nothing on stdout) -- this
is a clean refusal, not a crash or a silent zero.

`abs` is a stand-in: the refusal fires while marshalling the argument, before
anything is called, so the callee never matters. Any parametric-monomorph
field reproduces it; the shape is the point, not the type arguments.

## Root cause

`agg_field_class`, [src/turi/eval.c:1216](../../src/turi/eval.c) --

```c
static AggFieldClass agg_field_class(const CtorField *f, const AdtDef **out_def) {
    if (adt_field_is_inline_byval(f)) {
        if (f->full_type->kind == TY_ADT) { ... return AGGF_NESTED; }
        return AGGF_UNSUPPORTED;      /* <-- TY_APP lands here */
    }
    return AGGF_SCALAR;
}
```

`adt_field_is_inline_byval` ([src/compiler/types.c:2894](../../src/compiler/types.c))
admits exactly two kinds: `TY_ADT` and `TY_APP`. `agg_field_class` handles
`TY_ADT` as `AGGF_NESTED` and drops everything else into `AGGF_UNSUPPORTED`,
so **`AGGF_UNSUPPORTED` is reachable only for `TY_APP`**. The refusal is
raised at [src/turi/eval.c:1610](../../src/turi/eval.c).

### The diagnostic is inaccurate

> `call-ptr: arg 0's record has a field with no by-value C member type`

Because `AGGF_UNSUPPORTED` is reachable only through the
`adt_field_is_inline_byval` branch, every field that triggers this message
**does** have a by-value C member type -- `adt_field_is_inline_byval` returning
true is precisely the statement that codegen inlines it by value, and the
compiled half of the repro above proves it does. The true cause is "the
interpreter cannot render this field's layout without per-application type
substitution."

As written the message points at the user's type as malformed, when the type
is fine and the limitation is the interpreter's. Worth fixing even if the
underlying gap stays open -- it is a one-line wording change, and it is the
difference between "my struct is wrong" and "this path is not implemented."

## Fix directions

Two independent pieces, in increasing size:

1. **Reword the diagnostic** (cheap, do this regardless). Say what is
   actually true: the field is a parametric monomorph and the interpreter
   cannot render its layout; the compiled path supports it. Naming the
   offending field would help more than naming the arg index.
2. **Render the layout** (the real fix). `agg_field_class` needs to resolve a
   `TY_APP` to its monomorph `AdtDef` and substitute the application's type
   arguments through the field types before walking them. `type_adt_app_def`
   and `adt_app_is_byvalue_product` (used by `adt_field_is_inline_byval_d`
   right above) already do the resolution half; what is missing is the
   substitution so nested field *kinds* come out per-application rather than
   from the generic definition. Codegen's monomorph layout tables are the
   obvious donor -- the same "single source of truth" idea section 4 of the
   plan raised for the struct registry and F4 then dissolved by encoding
   layout inline. Mirror `AGGF_NESTED`'s recursion once the substituted
   fields are in hand.

## Not a blocker

Nothing depends on this. The compiled path -- which is what a built program
uses -- handles these fields correctly today; only `--interpret` and the REPL
are affected, and only for a record that both crosses an FFI boundary by value
and has a parametric-monomorph field. It is documented as a user-facing
limitation in
[docs/guides/ffi-guide.md](../guides/ffi-guide.md) ("a field of concrete
*parametric* monomorph type is refused under `--interpret` today"), so the
behavior is not a surprise -- only its diagnostic is.
