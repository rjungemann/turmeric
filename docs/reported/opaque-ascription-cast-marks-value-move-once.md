# Opaque ascription cast `(:: expr :T)` marks its result move-once

> **FIXED (2026-06-05).** Root cause confirmed and patched in
> `src/compiler/elab_types.c`: the `F_KEYWORD` branch of `type_expr_from_form`
> resolved a known `:Opaque` name to a `TY_STRUCT` with `copy_kind` **hardcoded
> to `CK_MOVE`**, instead of carrying the registered def's discipline like the
> `F_SYM` path (which goes through `type_struct(def)`). The fix copies
> `copy_kind`/`substruct` from the resolved def, so `(:: v :Name)` over an
> unrestricted opaque is now `CK_COPY` (freely reusable), while `:linear`/
> `:affine` opaques become `CK_LINEAR`/`CK_UNIQUE` and are still enforced under
> `-Xlinear`/`-Xsubstructural` -- the keyword path now matches the `F_SYM` path
> exactly. Regression fixtures: `tests/fixtures/opaque-ascription-copy-reuse`
> (unrestricted reuse passes) and `tests/fixtures/kleisli-arrow-instance`
> (reuses ascription-disambiguated arrows). The original report follows.

**One-line summary:** Constructing an *unrestricted* opaque value via the
ascription cast `(:: expr :SomeOpaque)` produces a binding the linearity
checker treats as affine/move-once -- reusing it raises `TUR-E0005`
use-after-move -- even though the opaque is declared with no substructural
keyword (not `:linear`, not `:affine`).

**Severity:** ergonomics / soundness-of-diagnostics gap. It is not a
miscompile (no bad code is emitted; compilation is *rejected*), but it forces
spurious single-use restructuring or carrier round-tripping on values that are
semantically copyable. It bites exactly the values you are *forced* to produce
by ascription -- e.g. disambiguating a nullary typeclass method between two
instances (`(:: (ident) :Kleisli)`), which is the documented resolution for the
two-instance ambiguity case.

## Minimal repro

```turmeric
(defopaque Box [A] :int)              ;; unrestricted opaque, NOT :linear/:affine
(defn unbox [b : Box] : int (:: b :int))

(defn main [] : int
  ;; (a) plain constructor value -- reuse is FINE
  (let [p (:: 7 :Box)]                ;; ... see note: see below
    ...)
  0)
```

Observed in practice with `stdlib/kleisli.tur` (`defopaque Kleisli [A B] :int`):

```turmeric
;; plain wrapped value: reuse OK
(let [g (kleisli add1)]
  (println (some? (k-apply g 1)))     ;; ok
  (println (some? (k-apply g 2))))    ;; ok

;; SAME carrier re-wrapped via ascription cast: reuse REJECTED
(let [a (:: (kleisli add1) :Kleisli)]
  (println (some? (k-apply a 1)))     ;; ok (first use)
  (println (some? (k-apply a 2))))    ;; TUR-E0005 use-after-move: 'a' moved
```

- **Observed:** the second use of `a` fails with
  `TUR-E0005 use-after-move: binding 'a' was moved and cannot be used again`,
  with the "moved here" note pointing at the *first* use.
- **Expected:** `a : Kleisli` is an unrestricted opaque (carrier `:int`); like
  the un-ascribed `g`, it should be freely reusable. No move tracking should
  attach to it.

The only difference between the two bindings is that `a` flows through the
`(:: ... :Kleisli)` ascription cast and `g` is returned directly from a `defn`
whose return type is `Kleisli`. So the ascription cast -- not the opaque type
itself -- is what attaches move-once semantics.

## Root-cause direction

The move tracking is keyed on the `(:: expr :Opaque)` cast node rather than on
the opaque's declared substructural class. The opaque-construction path appears
to mint the cast result as a fresh linear/affine resource unconditionally,
instead of inheriting "unrestricted" from the target `defopaque` (which carries
no `:linear`/`:affine` keyword). A direct `defn`-return of the same opaque type
does *not* go through that path, which is why `g` is unaffected.

Likely locations to inspect (by symbol, not yet line-pinned):
- the elaborator's handling of `Ascription`/`::` when the target type resolves
  to a `defopaque` head (the construction/coercion direction), and
- where the linearity/ownership pass seeds move-tracking state for newly
  constructed opaque values -- it should consult the opaque's substructural
  attribute (the same one that makes `Chan`/`AsyncChan` `:linear`) and seed
  "unrestricted = no tracking" when absent.

## Validation of a fix

- The `(::)`-then-reuse repro above must type-check and run (both `some?`
  lines print).
- `Chan`/`AsyncChan` (`defopaque ... :linear`) must *still* reject reuse -- the
  fix must distinguish unrestricted opaques from genuinely linear ones, not
  blanket-disable move tracking on all ascription casts.
- A focused fixture: wrap an unrestricted opaque via `(:: v :T)`, use it twice,
  expect success; wrap a `:linear` opaque via `(:: v :L)`, use it twice, expect
  `TUR-E0005`.

## Resolution

Fixed as described in the banner above. The `tests/fixtures/kleisli-arrow-instance`
fixture now *reuses* each ascription-disambiguated arrow across multiple
applications (no workaround needed), and `tests/fixtures/opaque-ascription-copy-reuse`
is a focused regression: an unrestricted `(defopaque Crate [A] :int)` ascribed
via `(:: ... :Crate)` is used twice and must type-check. Linear enforcement was
verified unchanged: a `:linear` opaque reused under `-Xsubstructural` still
raises `TUR-E0101`, via both the ascription and the plain (`F_SYM`) paths.

## Discovered / fixed

While implementing `docs/upcoming/category-arrowzero-implementation-plan.md`
(the Kleisli `Category`/`ArrowZero` example instance), 2026-06-05. Fixed the
same day.
