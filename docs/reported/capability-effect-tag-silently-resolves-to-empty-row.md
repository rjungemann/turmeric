# An undeclared capability tag in `#fx{...}` silently resolves to the empty row

**Severity: medium (silent loss of a checked guarantee).** A `#fx{...}` row
naming an effect that no `defeffect` in the compile declares is dropped at
resolution with **no diagnostic**, so the annotation becomes decorative and
`#fx{}` on its callers becomes a promise the compiler has quietly stopped
checking. Filed 2026-10-01, found while scoping `^capability` for `println`.

**Status: OPEN.** The behaviour is already *documented* as a trap in
[effects-system-guide.md:375](../guides/effects-system-guide.md), which is how
we know it has bitten before -- `#fx{Bt}` "sat decorative on the trail
mutators for a month" before `Bt` was declared. This report is the filing the
guide note never got, because the failure mode is worse than the note implies:
it is not only that the tag does nothing, it is that a *second* function's
`#fx{}` silently stops meaning anything.

## Minimal repro

`IO` is declared in `stdlib/effects.tur:29`, which is **not** autoloaded. So
in a file that does not pull it in:

```turmeric
(defn maybe-io    [] #fx{IO} : int 0)
(defn claims-pure [] #fx{}   : int (maybe-io))
(defn main        [] : int 0)
```

```sh
./build/tur --dump-effects check repro.tur
```

Observed:

```
defn maybe-io    : #{}
defn claims-pure : #{}
```

Exit 0, no warning, no error. Expected: either `maybe-io : #{IO}` and a
`TUR-E0009` on `claims-pure`, or -- failing that -- a diagnostic naming `IO`
as an unresolved effect.

The second line is the real defect. `claims-pure` declares `#fx{}` while
calling something annotated as an authority-holding function, and that is
exactly the check `^capability` exists to perform:

> **Propagated from the declared row.** [...] A `#fx{}` (pure) caller of an
> `#fx{FS}`-tagged function therefore fails effect-row checking with
> `TUR-E0009`, exactly like the built-in `#fx{Unsafe}`.
> -- effects-system-guide.md:340-348

With the tag dropped, the caller's `#fx{}` passes. The annotation that was
supposed to *buy* the check is what silently removed it.

## Root cause

Effect-row resolution maps an uppercase name in `#fx{...}` to a declared
effect and drops what it cannot find, rather than diagnosing it. `Unsafe` is
immune because it is compiler-known (`EFFECT_NAME_UNSAFE`,
`src/passes/effect.h:164`); every `^capability` tag in
`stdlib/effects.tur` depends on that file being in the compile.

This is why `Bt` is declared in `trail.tur` rather than `effects.tur` --
the guide says so outright (`effects-system-guide.md:355-358`): the module
that uses it is autoloaded and `effects.tur` is not. That is a workaround for
this defect, applied once, by hand, for one tag.

## Why it matters more now

Tagging `println` with `#fx{IO}` (see
[effect-row-honesty-plan](../upcoming/effect-row-honesty-plan.md)) walks
straight into this. `println` is a builtin available in every program;
`IO` is declared in a module most small programs never import. A `println`
tagged `#fx{IO}` would be a no-op in precisely the files that need it, and
every `#fx{}` in them would be an unchecked promise. The tag cannot be hung on
a builtin until resolution is honest.

## Fix directions

1. **Diagnose an unresolved effect name** (preferred). An uppercase name in
   `#fx{...}` with no `defeffect` in the compile is a hard error naming the
   tag, with a "did you import the module that declares it?" hint.

   **The sizing sweep is done (2026-10-01): zero undeclared effect tags** in
   `stdlib/` plus `tests/fixtures/` -- 146 files carry a `#fx{...}`, 28
   distinct names, every effect among them resolves. So this breaks no
   existing effect annotation.

   It does break something else. **Three names in `#fx{}` are not effects at
   all** but compiler attributes, interned by name in the elaborator rather
   than declared by `defeffect`, so a naive rule rejects them:

   | Marker | Interned at | Used by |
   | --- | --- | --- |
   | `Construct` | `src/compiler/elab_core.c:2398` | `ok`/`err` (`stdlib/result.tur:39,57`), `some` (`stdlib/option.tur:33`) |
   | `ByVal` | `src/compiler/elab_core.c:2400` | m5 by-value accessor marker |
   | `NonExhaustive` | `src/compiler/elab_structs.c:3568,3607` | `match` exhaustiveness opt-out |

   (`Unsafe` is a fourth, via `sym_effect_unsafe`, `elab_core.c:2243`.)
   Allowlist all four, and assert the allowlist is exhaustive against the
   interned set so a future attribute cannot silently re-become a dropped
   effect.
2. **Make the five stdlib capability tags compiler-known**, as `Unsafe`
   already is, so `IO`/`FS`/`Net`/`Proc`/`Rand` resolve without an import and
   `Bt`'s hand-placement stops being special. Does not fix `#fx{Typo}`;
   complements direction 1 rather than replacing it.
3. **Warn only**, as a transitional step, if direction 1's sweep comes back
   large. Weakest option: the whole problem is that silence here reads as
   success.

Directions 1 and 2 are independent and both wanted: 1 makes a typo loud, 2
makes the common tags work where they are needed.

## A related wart, worth its own change

`#fx{}` is doing two jobs: effect rows and compiler attributes. Every other
effect-system language keeps these apart -- in Koka, Unison, OCaml 5, Effekt
and the Haskell effect libraries an effect label is an ordinary type-level
name, so an unknown one is a plain unbound-identifier error and this failure
mode cannot arise; attributes live in separate pragma syntax (`{-# ... #-}`,
`[@@...]`, `@ann`).

Turmeric already has that separate syntax: `^<lowercase>`, ~1,100 uses in
`stdlib/` (`^fat`, `^borrow`, `^mut`, `^linear`, `^multishot`, `^private`,
`^deprecated`, and `^capability`/`^extends` on `defeffect` itself). The case
convention agrees -- effects uppercase, attributes lowercase -- and the three
markers above are uppercase only because `#fx{}` demanded it. The clean
spelling is `^construct` / `^byval` / `^non-exhaustive`.

Not folded into direction 1 because `#fx{NonExhaustive}` is **documented
user-facing syntax**, with its own section at
[`sum-types-guide.md:241`](../guides/sum-types-guide.md) and a line in that
page's description, so it needs a deprecation cycle rather than a rename.
Tracked in
[effect-row-honesty-plan](../upcoming/effect-row-honesty-plan.md) section 6
Q1.
