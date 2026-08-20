# No TMVar/TChan in stdlib -- STM docs must hand-sketch them

**Severity: low** (minor expressiveness). Found in the 2026-08-20 docs audit.

## Repro

`grep -rn tmvar stdlib/` -> nothing (only interned-but-unused
`sym_tmvar`/`sym_tchan` in src/compiler/elab_core.c:2079). Both STM docs
build the patterns from `tvar/*` + `check` by hand.

## Fix direction

`stdlib/stm-sync.tur` with tmvar-take/put and tchan built on the existing
forms.

## Guides to update when fixed

- docs/guides/stm-guide.md
- docs/guides/stm-tutorial.md
