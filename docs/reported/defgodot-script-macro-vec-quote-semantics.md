# `defgodot-script` Macro Fights List Quote-vs-Value Semantics on VEC Items

> **Status:** Reported, not yet investigated
> **Severity:** Medium -- macro authors building DSLs (e.g. the
> turmeric-godot `defgodot-script` family) that need to embed `VEC`
> literals / vector items inside macro-expanded forms cannot get the
> quote vs. value semantics to line up; the workaround so far has been
> to dodge VEC items in the DSL surface.
> **Discovered:** 2026-06-25
> **Discovered by:** agent attempting to build a `defgodot-script`
> macro with VEC items in its body, who reported "the macro path is
> fighting list's quote-vs-value semantics."

---

## Summary

When a macro body emits a list form that contains VEC items, the
quoting model used by the macro expander vs. the value-construction
model used by ordinary list literals do not line up. The macro author
ends up unable to decide a single posture -- quote the whole template
and lose access to value-built VECs spliced from the macro
environment, or build the list with value semantics and lose the
ergonomic literal form for the surrounding shape.

Concretely the reporting agent hit this trying to write something
along the lines of:

```turmeric
(defmacro defgodot-script [name & body]
  ;; wants to emit a form that includes VEC items inside, where some
  ;; VECs are literal and some come from the macro's lexical env
  ...)
```

and observed that whichever way the template was written, one of the
two VEC sources came out wrong: literal-shaped VECs were fine but
spliced VECs were quoted (or vice versa).

## Why this matters

`defgodot-script` is the kind of DSL we want spices (turmeric-godot,
turmeric-raylib, ...) to ship. If macro authors have to actively avoid
VEC items in the DSL surface to dodge a quoting-model gap, the macro
system is leaking implementation detail into every spice that wants a
declarative entry point. This is also a recurring shape -- any DSL
that wants record-ish / vector-ish literals in its surface will hit
the same wall.

## Repro direction (to confirm during the fix)

Minimal repro is "write a macro whose template body contains a VEC
literal and a spliced VEC binding side-by-side; observe that they
cannot both come out right under any single quoting choice."
A clean repro fixture under `tests/fixtures/macros/` would pin the
exact mismatch.

## Fix direction

Do a pass on macro expansion + list/VEC handling and pick a single
coherent story:

- Decide quote vs. value semantics for VEC items inside macro
  templates explicitly (not "whatever falls out of the list code").
- Make spliced VECs and literal-shaped VECs both expressible in a
  single template without the author having to switch modes.
- Land a fixture under `tests/fixtures/macros/` that pins the chosen
  semantics so this does not regress.

## Notes

- Not blocking the v1 track today (the workaround is "don't put VECs
  in macro templates"), but it is squarely in the "DSL spices ship on
  this" path, so worth resolving before we lean on `defgodot-script`-
  style entry points more broadly.
- The reporter did not pin down the offending file:line; a real
  investigation should start by writing the minimal failing macro and
  tracing where the VEC item loses (or keeps) its quote.
