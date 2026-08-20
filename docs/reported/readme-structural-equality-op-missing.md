# README documents a structural-equality operator =struct= that does not exist

**Severity: low** (docs advertise vaporware). Found in the 2026-08-20 docs
audit.

## Repro

`grep -rn '=struct=' src stdlib tests` -> zero hits; `git log -S'=struct='` ->
never existed. `(println (=struct= [1 2 3] [1 2 3]))` cannot compile. The
README carries both an example block and a "Structural equality (`=struct=`)"
features row.

## Root cause

README example added without an implementation. Structural equality exists
internally (e.g. `=` on Syntax values via form_equal at
src/turi/eval.c:3146, HAMT structural eq in src/runtime/hamt.h:271) but there
is no `=struct=` surface form.

## Fix direction

Either implement `=struct=` (or decide the real surface spelling for deep
structural equality on vec/map literals) or drop the README example + features
row.

## Guides to update when fixed

- README.md (example at "Structural equality" + features table row)
