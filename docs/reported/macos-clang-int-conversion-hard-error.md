# macOS clang rejects emitted `void*`<->`int64_t` straddles (`-Wint-conversion`), reddening the whole macOS CI leg

**Severity:** medium (CI-only, platform-specific; no correctness impact -- the
same emitted C runs fine where it compiles). It makes the `Test (macos-latest)`
CI job **permanently red** (`~94` fixtures FAIL as "build failed"), so a real
macOS regression can hide behind the existing red. Pre-existing on `main`
(identical failure set), independent of any one PR.

**One-line:** several emitted-C sites implicitly convert between `void*` and
`int64_t`; the macOS runner's Apple/LLVM clang treats `-Wint-conversion` as a
**hard error by default** (clang 15+), while the ubuntu runner's `cc` (GCC) only
warns -- so the identical generated C compiles on Linux and fails to compile on
macOS.

## Repro

Any fixture whose emitted C hits an unbridged straddle. Smallest reliable one:

```sh
tur emit-c tests/fixtures/string-basic/input.tur > /tmp/sb.c
clang -std=c99 -Wall -c /tmp/sb.c -o /tmp/sb.o     # no -Werror needed
```

```
/tmp/sb.c:4908:12: error: incompatible pointer to integer conversion returning
'void *' from a function with result type 'int64_t' (aka 'long') [-Wint-conversion]
    return (void*)tur_string_from_cstr(buf);
```

`gcc -std=c99 -Wall -c /tmp/sb.c` on the same file emits only a warning (or
nothing), which is why `Test (ubuntu-latest)` is green. Confirmed locally: the
in-container clang reproduces the CI error with no `-Werror`; `string-basic`
alone emits 11 such lines.

CI evidence: run #2015 (PR) and run #1999 (`main`) both fail
`Test (macos-latest)` -> "Run fixture suite" with `summary: 2149 passed, 94
failed`, the failures being this `-Wint-conversion` class plus a few stdout
mismatches (`vec-push-byvalue-aggregate-escapes-frame`, `show-string-owned-interp`,
`derive-show-string-interp`) that are likely a separate pre-existing item.

## Root cause (sites found)

`tur_string_from_cstr` returns `void*` (`src/runtime/tur_string.h:34`), but a
`String`-typed body lowers to an `int64_t`-returning C function. The inline-C
returns the pointer **without bridging through `(intptr_t)`**:

- `stdlib/typeclass-show.tur:50` (and the sibling `Show` instances at lines 59,
  68, 77, 86, 95, 104, 113, 122, 139, ...):
  `return (void*)tur_string_from_cstr(buf);`
  -> should be `return (int64_t)(intptr_t)tur_string_from_cstr(buf);`
  This one instance file accounts for the bulk of the failures (every fixture
  that shows an int / derives Show / interpolates a String pulls it in).

- `stdlib/taskgroup.tur:477` `return fiber;` (a `void*` fiber returned from an
  int64 function) and `stdlib/taskgroup.tur:636` `arg->tg = group;` (int64
  assigned to a `void*` field) -- the `taskgroup-*`, `httpd-async-*`,
  `session-*`, `reactor-*`, `schan-*` failures.

- A codegen straddle in the `vec_empty_like` monomorph path passes an `int64_t`
  to a `void*` parameter (`vec-captureless-fat-closure-readback`,
  `vec-multiword-struct-*`). This one is emit-side, not stdlib inline-C.

The build driver **used to** carry `-Wno-error=int-conversion` for the GCC-14
front (see the comment at `src/main.c:2134`); it was removed when the GCC fronts
were declared bridged (`docs/archive/codegen-gcc14-permerrors.md`). That closed
GCC but left the clang-default-error case open, and new/uncovered straddles
(the `typeclass-show` inline-C) regressed unnoticed because ubuntu CI is GCC.

## Fix directions

Two independent tracks, not mutually exclusive:

1. **Fix the sites (correct, portable).** Bridge each straddle through
   `(intptr_t)`: `stdlib/typeclass-show.tur` returns become
   `(int64_t)(intptr_t)tur_string_from_cstr(...)`; `stdlib/taskgroup.tur:477/636`
   cast through `(intptr_t)`; fix the `vec_empty_like` emit to bridge the
   element cast. Then the generated C compiles clean under clang's default
   `-Wint-conversion` and GCC `-Werror`. This is the real fix and is
   well-scoped (the `typeclass-show` change alone clears most of the 94).

2. **Stopgap (unblock the macOS leg now).** Append
   `-Wno-error=int-conversion` (and, symmetrically,
   `-Wno-error=incompatible-pointer-types`) to the emitted-program `cc`
   invocation next to the existing `-Wno-error=implicit-function-declaration`
   at `src/main.c:2145` and `src/main.c:4322`. This restores parity with the
   GCC-14 era and turns the macOS hard errors back into warnings, but it hides
   real straddles -- so it should be a bridge to track 1, not the destination.

Verify by re-running `Test (macos-latest)` (or locally:
`clang -std=c99 -Wall -c` over `tur emit-c` output for `string-basic`,
`taskgroup-linear`, `vec-multiword-struct-eq`) -> 0 `-Wint-conversion` errors.

Discovered while triaging CI on the `cps-reopen-perform-onode-leak` PR; the
leak fix is unrelated and its own CI legs (ubuntu fixture suite, codegen
snapshots) are green.

## Progress (2026-07-21): Track 1 sites fixed + emit-side String/witness bridges; ~94 -> 22

Landed the well-scoped Track-1 site fixes plus three emit-side bridges. A broad
clang sweep (`tur emit-c <fixture> | clang -std=c99 -Wall -c`) over the fixture
tree shows fixtures-with-straddles dropped from ~94 to **22**. Full gcc suite
green (2246 passed, 0 failed).

Fixed:

- **`stdlib/typeclass-show.tur`** (all 11 `Show [..] : String` inline-C returns):
  `return (void*)tur_string_from_cstr(...)` ->
  `return (int64_t)(intptr_t)tur_string_from_cstr(...)`. Clears the bulk named
  here (`string-basic`, `derive-show-string-interp`, `show-string-owned-interp`,
  etc.).
- **`stdlib/taskgroup.tur:477/636`**: `return fiber;` ->
  `return (int64_t)(intptr_t)fiber;`; `arg->tg = group;` ->
  `arg->tg = (void*)(intptr_t)group;`. Clears `taskgroup-*`.
- **Emit-side panic-temp return** (`emit_expr.c` record + `emit_fns.c` reverse
  return-bridge): a `void *`-returning extern ascribed to an opaque carrier
  (`String`) returned through the int64 slot now bridges. See
  `docs/reported/compiled-string-return-int-conversion.md` progress note.
- **Emit-side `vec_empty_like` witness arg** (`emit_expr.c`): int64 carrier passed
  to a `void *witness` param now bridges `(void *)(intptr_t)`. Clears
  `vec-captureless-fat-closure-readback`, `vec-multiword-struct-*`.

**Remaining 22 (report stays OPEN):** a distinct long tail of
carrier-representation straddles at OTHER positions -- int64 <-> a *concrete*
parametric-struct pointer (`tur_adt_Vec__int *`, `tur_adt_Map__String__int *`,
...) in arg / assignment / return / init position, plus void*<->int64 binder
inits. Sample fixtures: `string-int`, `schan-worker-pool` (6),
`show-wrapper-helper-dispatch` (3), `make-struct-parametric-fn-field-infer` (3),
`digest-hex` (2), `dot-parametric-fn-field-call` (2),
`show-collections-content-hamt` (3), `cloneable-owning-*`, `session-effects`,
`stdlib-lens-record-field`, `path-string`, `string-slice`, `string-map-key`,
`string-reader-macro`, `reactor-fibers-park-chan`. Each is a separate emit-site
carrier bridge (arg-coercion / binder-init / return position); they share the
`gcc14-int-conversion (carrier-representation-tracking)` machinery but are not the
String-return shape fixed above. The `-Wno-error=int-conversion` /
`-Wno-error=incompatible-pointer-types` stopgap in `src/main.c` therefore stays
until this tail is drained.
