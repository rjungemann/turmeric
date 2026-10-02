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
   tag, with a "did you import the module that declares it?" hint. This is a
   breaking change for any tree carrying a `#fx{Typo}` today -- worth a sweep
   of `tests/fixtures/` and `stdlib/` first to size it.
2. **Make the five stdlib capability tags compiler-known**, as `Unsafe`
   already is, so `IO`/`FS`/`Net`/`Proc`/`Rand` resolve without an import and
   `Bt`'s hand-placement stops being special. Does not fix `#fx{Typo}`;
   complements direction 1 rather than replacing it.
3. **Warn only**, as a transitional step, if direction 1's sweep comes back
   large. Weakest option: the whole problem is that silence here reads as
   success.

Directions 1 and 2 are independent and both wanted: 1 makes a typo loud, 2
makes the common tags work where they are needed.
