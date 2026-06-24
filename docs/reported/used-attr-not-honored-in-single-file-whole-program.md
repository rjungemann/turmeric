# `#[used]` is not honored on the single-file / whole-program build path (`tur test`, `tur run <file>`, `tur build <file>`)

**Severity:** low-medium. A `#[used]` defn reached **only** through its raw
mangled C symbol from a sibling module that is *not* `(import)`ed is dropped at
link time when the program is built single-file / whole-program. `tur build
<project>` (separate compilation) handles this case correctly; only the
single-file path regresses. In practice this is the legacy "hand-spelled raw
`extern`" pattern that the C-integration guide already steers away from
(`import` + `__TUR_CNAME_<name>__`), so guide-following spice code does not hit
it -- but the attribute's contract ("retain with external C linkage") is
silently not upheld on this path.

**Status:** open as of v0.25.0 (HEAD `6244033`).

## Summary

`#[used]` (`Binding.retain_c_linkage`, `src/compiler/expr.h:106-113`) tells the
compiler to keep an unexported, Turmeric-unreachable defn alive with external C
linkage, for defns reached only via their mangled C symbol (cross-module
inline-C bridges, by-address C-ABI callbacks).

In **project builds** (`cmd_build_project`, `src/main.c`), the whole-program
single-main shortcut is correctly disqualified when any non-entry module carries
`#[used]`: `file_has_used_attr` (`src/main.c:3514-3539`) is consulted at
`src/main.c:4298-4306`, and the build falls through to separate compilation,
which compiles and links **every** project module. This works (verified).

In **single-file / whole-program builds** -- `tur build <file>`, `tur run
<file>`, and `tur test <dir>` (which builds each test file via the single-file
`cmd_build` path) -- there is no such fallback. The whole-program emitter
inlines only the entry module's transitive **Turmeric import closure**
(`emit_program`, `ctx.separate_compilation = false`, `emit_module.c:7849`), so a
sibling module that is reached **only** through a raw `extern <mangled>`
reference -- and therefore is *not* in the import closure -- is never inlined,
never emitted, and dangles at the C link step.

Note the boundary: when the sibling module **is** in the import closure (the
normal case -- the importing module `(import)`s it and uses its public API),
the `#[used]` defn *is* retained even though it is unexported and
Turmeric-unreachable, and it links fine as a same-TU `static` (verified, see
"What works" below). The gap is specifically the *no-import* raw-extern case.

## Repro

This is the exact shape of the canonical `#[used]` test in
`tests/run-build-project.sh:811-863` (`app/a`'s `__helper`, reached only via its
mangled symbol, with **no** `(import app/a)`):

```
/tmp/used/build.tur
  (defpackage tur-used-attr :name "tur-used-attr" :version "0.1.0"
    :exports #{ "app/main" ["main"]  "app/a" [] })

/tmp/used/src/app/a.tur
  (defmodule app/a
    (defn #[used] __helper [x : int] : int
      ```c
      return x + 1;
      ```))

/tmp/used/src/app/main.tur
  (defmodule app/main
    (defn use [x : int] : int
      ```c
      extern int64_t app__a____helper(int64_t);
      return app__a____helper(x);
      ```)
    (defn main [] : int (use 41)))
```

```sh
# project build -- separate compilation, #[used] honored: OK
tur build /tmp/used -o /tmp/used/proj && /tmp/used/proj; echo $?   # => 42

# whole-program single-file (the `tur test` / `tur run <file>` path): FAILS
tur build /tmp/used/src/app/main.tur -I /tmp/used/src -o /tmp/used/wp
# /usr/bin/ld: undefined reference to `app__a____helper'
# tur: cc invocation failed (status 256)
```

## What works (the boundary)

If `app/main` `(import app/a)`s the sibling (so it joins the import closure),
the `#[used]` helper is retained even under whole-program and links as a
same-TU `static` -- whole-program does **not** prune non-exported defns from an
inlined module. So the gap is **only** the no-import raw-extern pattern, not
`#[used]` in general. This is why the real `frame` spice tests are unaffected:
they `(import)` the module under test, so its `#[used]` Arrow-release callbacks
stay in-closure. (The `frame` link failures were a separate, spice-side issue --
stale hand-spelled mangled names -- already resolved; see
`docs/archive/cross-module-private-helper-dropped-at-link.md`.)

## Root cause

`file_has_used_attr` (`src/main.c:3514`) is only consulted by
`cmd_build_project` (`src/main.c:4298-4306`). The single-file entry points
(`cmd_build` for `tur build <file>`; `cmd_test` -> `cmd_build`; `tur run
<file>`) never check it, and the whole-program emitter has no notion of "retain
this module even though nothing imports it." So a `#[used]` defn in an
unimported sibling is dropped before it is ever emitted.

## Fix directions

Two independent shortfalls, either of which closes the gap:

1. **Inclusion** (the real drop): on the single-file / whole-program path, when
   `-I`-reachable sibling modules carry `#[used]` defns, force those modules
   into the inlined set so the symbols are emitted. The cheapest form mirrors
   the project-mode guard: have `tur test` / `tur run <file>` detect `#[used]`
   in the include search path and route through separate compilation
   (`cmd_build_multi_files`) instead of single-file whole-program. Scope: the
   single-file path has no manifest-driven module set today, so this needs an
   `-I`-dir scan or a `tur test`-level project-context hook.

2. **Linkage** (consistency, secondary): even for in-closure `#[used]` defns the
   whole-program path emits them `static` (the linkage guard at
   `emit_fns.c:525-529` and `emit_module.c:4486-4489` is gated on
   `ctx->separate_compilation`). Same-TU references resolve regardless, but the
   attribute's documented meaning ("external C linkage") is not upheld, which
   matters for `tur build --shared` whole-program outputs or any host C that
   looks the symbol up by name. Dropping the `separate_compilation` gate for
   non-stdlib `retain_c_linkage` defns would make `#[used]` mean the same thing
   in both build modes (the `is_from_stdlib` guard must stay to avoid duplicate
   external stdlib symbols).

## Workaround (no compiler change needed)

Follow the documented pattern: `(import)` the sibling module and reference its
defn through the module system or the `__TUR_CNAME_<name>__` splice rather than
a hand-spelled raw `extern`. With the import present the module joins the
closure and the `#[used]` defn is retained on every build path. See
`docs/guides/c-integration-guide.md` ("Calling a sibling `defn` from inline C").
