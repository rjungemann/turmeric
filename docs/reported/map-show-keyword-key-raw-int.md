# Show over `:Sym` renders the raw carrier int in generic / auto-show paths

**Severity:** low (display only; the values themselves are correct).

## Status (2026-07-26): partially fixed

- **FIXED -- missing `Show [Sym]` instance.** There was no `Show` instance for
  `:Sym` at all (only `Eq`/`Hash`/`MapKey[Sym]` in `sym.tur`). Added
  `Show [Sym]` (renders `":name"` via `sym->str`) to `stdlib/typeclass-show.tur`
  -- Show's own module, since `sym.tur` loads before the `Show` class is
  defined and an instance there could not name `Show`. The body is ordinary
  Turmeric (no inline-C) so the tree-walking interpreter can run it via the
  `sym->str` native. Zero fixture churn (instances emit lazily). Regression
  fixture: `tests/fixtures/show-sym-instance/`.

  Now working:
  - Direct show, interpreter and compiled: `(show :hello)` => `":hello"`.
  - Typed collection show in the interpreter:
    `(show (:: #map{:a 1 :b 2} (Map Sym int)))` => `"#map{:b 2 :a 1}"`;
    `(show (:: #set{:x :y} (Set Sym)))` => `"#set{:x :y}"`.

- **FIXED (2026-07-29) -- root cause B, the interpreter/REPL auto-show path.**
  See [Resolution: root cause B](#resolution-2026-07-29--root-cause-b).

- **STILL OPEN -- root cause A**, the compiled path's carrier collapse. Its
  blast radius turned out to be **narrower than this report originally
  claimed**; see [Root cause A](#root-cause-a--compiled-generic-show-binds-showint-for-a-sym-element).

## Repro of what remains

Root cause A only -- the compiled path:

```
;; compiled generic collection show over Sym
(show-line (:: (vec-of :a :b) (Vec Sym)))   => [4343871968 4343871992]
(show-line (:: (vec-of "x" "y") (Vec cstr))) => [x y]       ; distinct carrier: fine
```

The bare-prompt cases below were the root-cause-B symptom and now render
correctly:

```
turi> #map{:a 1}          => #map{:a 1}
turi> #map{"s" 1}         => #map{s 1}
turi> #map{7 70}          => #map{7 70}
turi> #set{:x :y}         => #set{:x :y}
turi> (vec-of :a :b)      => [:a :b]
```

## Root cause A -- compiled generic show binds `Show[int]` for a Sym element

`:Sym`'s carrier is `int64_t` -- *identical to `int`'s*. When a generic
constraint site (`vec-show-loop [^Show A]` / `map-show-loop [^Show K ^Show V]`)
is monomorphized for a Sym element, the element type collapses to its `int64_t`
carrier and instance selection binds `Show[int]` instead of `Show[Sym]`. Types
with a distinct C carrier (`cstr` -> `const char *`, `bool`, `float`) resolve
correctly.

### Correction (2026-07-29): the blast radius is narrower than first stated

This report originally generalised the above to "only `int64_t`-carried types
(Sym, and by extension `defopaque`-over-int / ADT-as-int) collide with `int`."
**The `defopaque` half of that is wrong**, and it matters, because it was the
part that made this look like a broad instance-selection defect. Measured:

```turmeric
(defopaque UserId :int)          ; int64_t carrier, exactly like Sym
(definstance Show [UserId] (show [x] : String (string/from-cstr "U#")))
(show-line (:: (vec-of (mk-uid 7) (mk-uid 8)) (Vec UserId)))
;; => [U# U#]     -- CORRECT; does NOT collapse to Show[int]
```

So a user `defopaque` over `:int` selects its own instance fine. `Sym` is not a
`defopaque` -- it is a builtin type kind (`TY_SYM`, `src/compiler/types.h:180`),
which is what puts it on a different path. The visible mechanism is in the
emitted C: a `(Vec cstr)` program emits a specialization
`__inst_Show_show_Vec__spec__int64_t_tur_adt_Vec__cstr__`, while the `(Vec Sym)`
program emits **no** corresponding spec and falls back to the generic
`__inst_Show_show_Vec`, which binds `Show[int]` -- even though
`__inst_Show_show_Sym` exists in the same translation unit. Specialization names
are mangled through `type_c_name` (`src/compiler/emit_module.c:1577-1580`),
i.e. through the **C carrier**, which is where `Sym` and `int` become
indistinguishable.

Before treating this as a general monomorphization defect, re-measure which
types actually reach it; on current evidence it is `TY_SYM` and whatever else
shares a builtin carrier name, not every `int64_t`-carried type.

Evidence -- emitted C for `(show-line (vec-of :a :b))` (Sym-only program):

```c
static int64_t vec_hyshow_hyloop(int64_t v, int64_t i, int64_t len, int64_t b) {
    ...
    __auto_type __ps_220 = (__inst_Show_show_int(__ps_219));   // <- Show[int], not Show[Sym]
    ...
}
```

This is the hybrid carrier/by-value monomorphization gap -- see
`docs/guides/monomorphization-abi-guide.md`. The fix is compiler-side (carry
the source type, not the carrier, into generic-instance selection), not a
stdlib instance.

## Root cause B -- interpreter auto-show erases collection element types

The WASM/REPL bare prompt renders a collection result through
`turi_try_show_by_tag` -> `turi_call_show_named(env, "Map", val)`
(`src/turi/eval.c:11484`, `:11392`), which invokes the generic `Show [Map]`
instance on a runtime value that carries no element types. Its `(Show K)` /
`(Show V)` constraint dicts default to `Show[int]`, so every non-int key/value
-- Sym AND `cstr` -- prints as the raw carrier int. Only `int` is right, by
coincidence of carrier == value. This is why the bare-prompt symptom is not
Sym-specific and is not fixed by adding `Show[Sym]`: the auto-show never learns
the keys are Syms. A real fix threads the elaborated element types (the result
expression's full `(Map Sym int)` type, not just the `"Map"` head tag) into the
per-element dict selection.

## Fix directions

- (B, targeted) **Done** -- see below.
- (A, structural) Fold into the end-to-end monomorphization work so a generic
  `^Show A` site selects the instance by source type. Note the corrected blast
  radius above: this is not "every `int64_t`-carried element type."

## Resolution (2026-07-29) -- root cause B

Taken as the targeted direction: the full result type is retained beside the
head-only tag, and used to seed the instance's constraint tyvars.

### Why the elements were erased

The interpreter has **no dictionary passing**. Elaboration bakes one instance
into each method call site, and the interpreter re-resolves at runtime by
either (i) substituting a tyvar pinned on the frame chain, or (ii) dispatching
on the receiver's runtime tag. When the receiver's type is a bare tyvar, the
elaborator bakes the *int-carrier representative* -- literally the instance
whose `type_args[0].kind == TY_INT` (`src/compiler/elab_typeclasses.c:5868-5921`).

`Show [Map]`'s body shows each element through `(show (:: ... K))`
(`stdlib/typeclass-show.tur:217-234`), and that ascription re-resolves only if
`K` is bound in the frame chain. Both recovery routes were unavailable to a
call synthesised from C:

- route (i) needs `frame->tyvars`, populated from a call site's `abi_bindings`
  -- and `turi_call` -> `eval_apply` binds parameters only, so a hand-built
  call has nothing to pin from;
- route (ii) explicitly bails on `TURI_INT` (`src/turi/eval.c`,
  `gde_reresolve_method_by_value`) because Vec/Map/Set/Sym/opaque all ride the
  int64 carrier and the tag cannot tell them apart.

So `Show[int]` ran for every element. This is why the symptom was **not
Sym-specific** -- a `cstr` key, which has its own carrier and is immune to root
cause A, misprinted identically. That divergence is the cleanest way to tell
the two root causes apart, so the regression test asserts it directly.

### The fix

`env->last_result_type` retains the full elaborated `Type` beside the existing
head-only `type_tag`; both are set from the same expression in
`turi_eval_impl`, so they cannot describe different results.
`turi_try_show_by_tag` pairs them (re-checking that the retained type's head
still matches the tag, so drift degrades to the old behavior rather than
misbinding), and `turi_call_show_named` seeds a synthetic parent frame via the
existing `frame_bind_instance_constraint_tyvars` -- the same helper the normal
dispatch path uses, reading each constraint's `param_idx` off the receiver's
type args. `frame_lookup_tyvar` walks the parent chain and `eval_apply_driven`
parents the callee frame to `cl->captured`, so the binding is visible through
the instance body and the nested `map-show-loop` call picks `K`/`V` up through
`frame_record_abi`.

No signature changes to `turi_call`; a caller with only a head tag passes NULL
and gets the previous behavior.

### Coverage

`tests/turi/show-collection-elems.c` (ctest target `tur_show_collection_elems`).
A `.tur` fixture cannot gate this -- the defect is in the auto-show display
path, and a fixture that writes `(show-line (:: m (Map Sym int)))` supplies the
element types itself and passes either way. The test drives
`turi_eval_typed` + `turi_try_show_by_tag` as `repl.c` and `wasm_glue.c` do.

Verified to gate: with the seeding disabled, **5 of 7 checks fail**. The two
that still pass are the `int` cases -- which is the point, since those were only
ever right by the coincidence of carrier == value.

### Verification

- `bash tests/run.sh` -- 2412 passed, 0 failed.
- `bash tests/run-turi.sh` -- 1668 passed, 0 failed, 690 skipped.
- `ctest -R 'tur_show_collection_elems|tur_lang_switch_prelude|tur_eval_basic|tur_repl'`
  -- 11/11 passed.

Measured on macOS (Darwin 27), Debug build with ASan+UBSan.

### Not addressed here

The **web display tiers** still need the same pairing: `wasm_glue.c` calls
`turi_try_show_by_tag` at two sites and will pick the fix up automatically for
any path that goes through `turi_eval_typed`, but this has not been rebuilt or
exercised under emscripten yet. Tracked in
`docs/reported/web-repl-lang-switch-drops-stdlib.md`, whose remaining open item
is exactly that routing.
