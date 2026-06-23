---
title: A `defdata` sum as a `derive-json` struct field decodes to the wrong instance (generic `decode` at the enclosing struct's result type, not the field's `Decode` instance)
category: Carrier <-> Concrete ABI -- dispatch re-resolution, sum-typed (defdata) struct field, return/decode direction
severity: Medium. A struct field whose type is a `defdata` sum has its
  `derive-json` `Decode` body dispatch the field's decode to the GENERIC
  `decode` class method specialized to the ENCLOSING struct's result type
  (`__inst_Decode_decode_T__spec__Result__Event__cstr_...`) instead of the
  field type's own `Decode [Cmd]` instance. A nested STRUCT field resolves
  correctly (`__inst_Decode_decode_Point`), so this is specific to sum-typed
  fields. Hard C error: `incompatible type for argument 1 of
  'ok_val__spec__int64_t_Result__Cmd__cstr'`. Blocks derive-json over structs
  with `defdata`-sum fields.
status: RESOLVED 2026-06-21 (gap G7). Fixed on
  branch claude/g2-carrier-concrete-abi-audit-3yzkhm; VERIFIED end-to-end against
  the real turmeric-spices json/encode derive-json (compiles, links, and runs:
  an `Event { cmd : Cmd }` round-trips `{"id":7,"cmd":{"Quit":{}}}`). See
  "Resolution" below.
---

## Resolution (2026-06-21)

Fixed and verified against the real `json/encode` derive-json (the
turmeric-spices sibling was cloned into the environment; `yyjson` built; the
emitted C compiles, links, and runs correctly -- `(decode doc root) : (Result
Event cstr)` over `{"id":7,"cmd":{"Quit":{}}}` yields `ok`).

Root cause was NOT in `emit_reresolve_disp_type` (the decode call is
return-dispatched and carries NO dict_arg, so the re-resolver early-returns).
The field decode `(:: (decode doc val) (Result Cmd cstr))` carries its class var
in `call->type` (`(Result a cstr)`); inside the enclosing `Decode [Event]` spec,
the result-recovery heuristics in `emit_abi_register_call` (the
`family_elem_rehydrated` / return-tyvar recovery) ground that class var to the
ENCLOSING element, so the call interned + recorded at `decode_T__spec__Result__
Event` -- an ill-typed sibling whose body is actually the `Cmd` decode (it
returns `Result__Cmd` from a `Result__Event` signature).

Fix (`emit_module.c`): when scanning an `EX_ASCRIBE` whose inner is a
return-polymorphic (result mentions a tyvar), dict-less global call with a
CONCRETE result ascription, register that call with the ascription's concrete
result as the `result_type_override` -- and *instead of* the plain inner scan,
so the correct (and only the correct) spec is interned + recorded. The two
result-recovery blocks in `emit_abi_register_call` now also bail when a
`result_type_override` is supplied, so the ascription's concrete result is not
clobbered back to the enclosing element. The `.cmd` field now decodes via
`decode_T__spec__Result__Cmd` (type-consistent with its `ok_val__spec__...
Result__Cmd` wrapper).

Suite green (1744/0). The promotable fixture is a `json` spice test (a
`derive-json` struct with a `defdata`-sum field, round-tripped) -- it lives in
the turmeric-spices repo, not the turmeric compiler suite (it needs yyjson).

---

# A `defdata` sum struct field decodes to the wrong instance

## One-line summary

In a `derive-json` `Decode` body, a field whose type is a `defdata` sum
dispatches its `(decode ... (Result Cmd cstr))` to the generic `decode` class
method carried at the *enclosing* `Result__Event` spec, not to the sum's own
`Decode [Cmd]` instance. A struct-typed field resolves correctly, so the bug is
specific to sum-typed (ADT) fields.

## Repro

```turmeric
(defdata Cmd :copy [] (Move :cstr :int) (Quit))
(derive-json-sum Cmd (Move (dir cstr) (steps int)) (Quit))
(defstruct Event [id : int  cmd : Cmd])
(derive-json Event (id int) (cmd Cmd))
;; (decode doc root) : (Result Event cstr)  on  {"id":7,"cmd":{"Quit":{}}}
```

`error: incompatible type for argument 1 of 'ok_val__spec__int64_t_Result__Cmd__cstr'`.
Emitted decode body for `Event`:

```c
.cmd = ok_val__spec__int64_t_Result__Cmd__cstr(
         __inst_Decode_decode_T__spec__Result__Event__cstr_int64_t_int64_t(   // (!) decode_T, Event's result
           doc, json_obj_get(doc, val, "cmd")))
```

Contrast -- a nested **struct** field (`Line { from : Point }`) emits the right
instance:

```c
.from = ... __inst_Decode_decode_Point(doc, json_obj_get(doc, val, "from")) ...
```

So `__decode-field`'s
`(ok-val (:: (decode doc (json-obj-get ... "cmd")) (Result Cmd cstr)))` picks
`__inst_Decode_decode_Point`-style dispatch for a struct field but falls back to
the generic `decode_T` (carried at the *enclosing* `Result__Event` spec) for a
sum field. Standalone `Decode [Cmd]` works (round-trip-sum passes), so the
instance exists -- it just isn't selected from inside another instance's body
for a sum-typed field.

## Root cause (direction)

This is a **dispatch re-resolution** gap (audit site 8 / `emit_reresolve_disp_type`,
`src/compiler/emit_core.c:1141`), on the **return/decode** side. The
`(decode ... (Result Cmd cstr))` call is return-dispatched: its class var
appears only in the result, recovered by `emit_pattern_extract_classvar` from
the ascription `(Result Cmd cstr)`. For a struct field the recovered type
(`Point`) selects `__inst_Decode_decode_Point`; for a `defdata` sum field the
recovered `Cmd` is **not** taking precedence over the enclosing spec's result
type (`Result__Event__cstr`), so the call stays pinned to the generic
`decode_T` method specialized at the enclosing instance.

The decisive difference from a struct field is the kind of the recovered type
(TY_ADT for a `defdata` sum vs TY_STRUCT for a nested struct): the resolver's
ascription-over-enclosing-spec precedence, or the concrete-instance lookup
(`emit_concrete_inst_method_fndef` / `emit_inst_head_matches`), is not matching
the sum's instance head.

## Fix direction

In the `Decode` body, dispatch a sum-typed field's
`(decode ... (Result Cmd cstr))` to that sum's `Decode [Cmd]` instance, the same
way a struct-typed field already resolves to `Decode [Point]`. The return-type
ascription names `Cmd`, so the resolver must let that ascription win over the
generic `decode` method specialized at the enclosing instance's result type --
i.e. the ascription-derived dispatch type takes precedence, and the concrete
instance lookup must match a TY_ADT instance head, not only TY_STRUCT.

## Related

This is the **sum-typed (defdata) struct field** case of the dispatch
re-resolution family in
`docs/carrier-concrete-abi-crossing-audit-plan.md` (gap G7), on the same routine
as G2 (nested parametric element) and G3 (by-value struct-field receiver):

- **G3** is a by-value struct-field *receiver* whose method takes the carrier
  param; **G7** is a struct-field whose *return-dispatched* decode picks the
  wrong instance entirely (generic method at the enclosing result type). Both
  are `emit_reresolve_disp_type` not honoring the field's true type over the
  enclosing spec -- G3 on the argument/receiver side, G7 on the
  return/ascription side.
- The fix belongs *in* `emit_reresolve_disp_type` (ascription-derived dispatch
  type wins; concrete-instance lookup matches TY_ADT heads), per the chokepoint
  discipline of
  `docs/carrier-crossing-recovery-routing-plan.md` (R1 dispatch side), not as a
  derive-json-specific patch.

## Validation

When fixed, the `Event { cmd : Cmd }` round-trip decodes via
`__inst_Decode_decode_Cmd` and the sum-field cell promotes to a fixture.
Cross-reference: gap G7 in `docs/carrier-concrete-abi-crossing-audit-plan.md`,
dispatch-side chokepoint in `docs/carrier-crossing-recovery-routing-plan.md`.
