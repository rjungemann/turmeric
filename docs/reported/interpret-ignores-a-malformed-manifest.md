# `tur --interpret` ignores a malformed `build.tur` entirely

**Severity:** low-medium. Not a wrong answer, but a parity gap that hides a
real configuration error: the compiled path refuses to continue on a manifest
it cannot read (TUR-E0624, exit 1), and the interpreter runs the program as if
no manifest existed, exit 0.

## Repro

```sh
mkdir -p /tmp/p2
printf '(defpackage broken :name\n'   > /tmp/p2/build.tur   # unterminated list
printf '(defn main [] : int 0)\n'     > /tmp/p2/input.tur

./build/tur check     /tmp/p2/input.tur   # error: unterminated list ... ; exit 1
./build/tur --interpret /tmp/p2/input.tur # (no output); exit 0
```

The breakage does not have to be exotic. An unterminated list is the plainest
malformed manifest there is, and it is what shows this is about manifest
handling generally rather than any one directive.

## Why it matters

The manifest is what puts the spice's own `src/` on the module search path and
declares its `:spices` deps. When it cannot be read, the compiled path is
explicit that nothing it declares is in effect and stops, precisely so a
half-configured search path does not produce a confusing downstream error. The
interpreter skips that check, so a program that imports a sibling module can
fail under `--interpret` with an unrelated "unknown name" while the real cause
-- a typo in `build.tur` -- is never mentioned.

It also means an `errors/` fixture asserting manifest diagnostics needs
`requires.compiled`; `tests/fixtures/errors/lang-trailing-token-manifest`
carries one for this reason.

## Root cause (where to look)

`pkg_manifest_read` / the walk-up discovery in `src/compiler/pkg.c` is reached
from the compiled entry points and from the per-file commands (`tur check`,
`tur emit-c`, `tur run <file>`), which is why those report it. The
`--interpret` entry point in `src/main.c` does not run the same discovery
before handing the blob to `turi_eval_file`, so `PKG_MANIFEST_MALFORMED` is
never produced and the TUR-E0624 gate never fires.

## Fix directions

Run the same discovery-and-validate step on the `--interpret` path and honour
`PKG_MANIFEST_MALFORMED` with the existing TUR-E0624 diagnostic, so the two
paths agree on what an unreadable manifest means. The auto-spice opt-out
(`--no-auto-spice` / `TUR_NO_AUTO_SPICE=1`) should suppress it on both arms
equally.

## Found

While adding `errors/lang-trailing-token-manifest` during the `#lang` layer
decommission (v0.49.0). Pre-existing and unrelated to that change -- it
reproduces on any malformed manifest, as above.
