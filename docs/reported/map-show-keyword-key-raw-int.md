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

- **STILL BROKEN -- two deeper, independent root causes below.** The instance
  is necessary but not sufficient for the report's original bare-prompt symptom.

## Repro of what remains

```
;; WASM/REPL bare prompt (auto-show): keys/values still raw ints
turi> #map{:a 1}          => #map{32697556 1}      ; want #map{:a 1}
turi> #map{"s" 1}         => #map{32702428 1}      ; string too -- not Sym-specific
turi> #map{7 70}          => #map{7 70}            ; int happens to be right

;; compiled generic collection show over Sym
(show-line (:: (vec-of :a :b) (Vec Sym)))  => [4303567136 4303567160]
```

## Root cause A -- compiled generic show binds `Show[int]` for a Sym element

`:Sym`'s carrier is `int64_t` -- *identical to `int`'s*. When a generic
constraint site (`vec-show-loop [^Show A]` / `map-show-loop [^Show K ^Show V]`)
is monomorphized for a Sym element, the element type collapses to its `int64_t`
carrier and instance selection binds `Show[int]` instead of `Show[Sym]`. Types
with a distinct C carrier (`cstr` -> `const char *`, `bool`, `float`) resolve
correctly; only `int64_t`-carried types (Sym, and by extension `defopaque`-over-
int / ADT-as-int) collide with `int`.

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

- (B, targeted) Preserve the full result type through `turi_eval_typed`'s
  `type_tag` (or a parallel channel) and have `turi_call_show_named` build the
  element dicts from the concrete `K`/`V` rather than defaulting to int.
- (A, structural) Fold into the end-to-end monomorphization work so a generic
  `^Show A` site selects the instance by source type, fixing every
  `int64_t`-carried element type at once.
