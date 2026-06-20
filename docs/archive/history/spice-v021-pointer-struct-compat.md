# Spice v0.21 compat: `sizeof` / `Struct-field` accessor / `float64*` -- not regressions

Severity: low (discoverability/migration, not a correctness bug).
Verified on: turmeric 0.21.0 @ 48e99d9, built from source; reproduced against a
fresh `turmeric-spices` checkout (`spices/linalg`, @ 257a12c).

## Question (from turmeric-spices Track C, porting `linalg`)

Three forms legacy C-backed spice code relies on fail with a bare
`unknown function or operator`:

1. `(sizeof X)` / `(sizeof :int)`
2. `defstruct` no longer emits `(Struct-field obj)` accessor *functions*
   (e.g. `(pt-x p)`)
3. `(float64* ptr i)` / `(float32* ptr i)` raw-pointer indexing

Asked: intentional removal, or regression?

## Finding: intentional -- these were never Turmeric language forms

Not a v0.21.0 removal. Verified by git archaeology on `src/`:

- `git log -S 'float64*'` over **all** history (all paths): **zero** hits.
  `float64*`/`float32*` never existed anywhere in this compiler's lineage.
- `declare` is not a special form -- `(declare malloc free)` parses as an
  ordinary call, so its args (`malloc`, `free`) error as `TUR-E0003 unbound
  symbol`. The supported way to name a C symbol is `(extern-c ...)`.
- `sizeof` only ever appears inside `\`\`\`c ... \`\`\`` inline-C blocks; it is
  not an expression-level operator.
- struct field reads are `(.field obj)`; `make-struct` constructs. No accessor
  *functions* are generated.
- The CHANGELOG `[0.21.0]` section (sized-types-on, Vec `:heap` ABI) lists none
  of these as removed -- consistent with "never existed."

`linalg` as written does not even parse/check against this compiler (declare ->
unbound `malloc`; `small.tur:20` old defstruct field syntax; pre-existing paren
imbalances at `solve.tur:177`, `decomp.tur:185`, `fmt.tur:188`). It targets a
legacy/imagined API, not a v0.21.0 regression.

## The "read-through-borrow" design blocker (Report #1) -- also not a gap

Report #1 claimed reading `.data` through a `^&mat` borrow errors `no typeclass
method found for 'data'`, framed as load-bearing design work. Root cause: wrong
borrow spelling. `^&mat` / `&mat` as a *type* is not the receiver-borrow form;
the supported form is the `^borrow` parameter annotation. This type-checks and
runs:

```turmeric
(defstruct mat [rows : int  cols : int  data : (Vec float)])
(defn mat-get [^borrow m : mat  i : int] : float (vec-get (.data m) i))
(defn mat-set! [^borrow m : mat  i : int  v : float] : void (vec-set! (.data m) i v))
```

Verified end-to-end (get + set! through the borrow, repeated use of the same
`m`, correct output). The full `linalg` target model (typed struct over a heap
`(Vec float)`) is a mechanical migration, not a blocked design problem.

## Resolution (shipped)

- Targeted migration diagnostics: an unknown-call-head on `sizeof`,
  `float64*`/`float32*`, `declare`, or a `<Struct>-<field>` accessor now carries
  a `help:` line pointing at the supported form, on both the compiled
  (`elab_call.c`) and interpreter (`turi/eval.c`) paths.
  Helpers: `tur_legacy_form_hint` + `struct_accessor_hint`.
- Migration section + error table added to
  `docs/guides/structs-guide.md` ("Migrating legacy `:int`-pointer struct code").
- Fixtures: `tests/fixtures/errors/legacy-pointer-form-hint`,
  `tests/fixtures/errors/struct-accessor-fn-hint`.

The `linalg` rewrite itself is turmeric-spices-side work (fold into U4 per
Report #1's own recommendation); nothing further is owed on the turmeric side.
