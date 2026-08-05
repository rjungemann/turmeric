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

## Progress (2026-07-21, cont.): tail 22 -> 14 across three well-scoped fixes

Drained 8 more of the tail. Root-caused the whole remainder to a single
principle -- **the emit bridge must cast to the TARGET slot's actual C type, not
a hardcoded `int64_t` (or `void *`) carrier word.** Full suite re-run
(`bash tests/run.sh`, 12-min timeout): **2244 passed, 5 failed**, and all 5
failures reproduce identically on a stashed baseline build (they are pre-existing
macOS-only issues -- `pipe2` static-vs-nonstatic collision in `hrt-stdlib-cont`,
`OSByteOrder.h` "function definition not allowed here" in the three `image-*`
fixtures, and the report's already-noted `vec-push-byvalue-aggregate-escapes-frame`
stdout mismatch). Zero regressions from these changes.

Fixed (each cleared fixture verified compile-clean under Apple clang AND
run-correct against `expected.stdout`):

- **Shape A -- cps->direct mono-clone return into a concrete-pointer binder**
  (`src/compiler/emit_cps_ir.c`, CT_LETCALL `mclone_lc` arm, ~line 5251). Was
  unconditionally `%s = (int64_t)(intptr_t)%s(...)`; the binder's declared C type
  (line 5644 uses `binder_ctype_full`) can be a concrete `tur_adt_Map__String__int
  *`, so the int64 cast straddled the pointer assignment. Now bridges to the
  binder's own C type when that is `int64_t` or a pointer (aggregate binders fall
  back to a raw assign). Clears `string-int`, `path-string`, `string-slice`,
  `string-map-key`, `string-reader-macro`.
- **Shape B -- monomorph ctor fn-field arg** (`src/compiler/emit_expr.c`, the
  `suffix && ... fields[i].kind == TY_FN` arm, ~line 4667). Was always
  `(int64_t)(intptr_t)(arg)`, but the monomorph ctor's fn-field PARAM type is
  `adt_field_c_type(def, fld, args)` -- `void *` for a BOXED fn field
  (`ctor_Lens__Person__cstr(void *, void *)`) and only `int64_t` for a carrier fn
  field (`ctor_Endo__int(int64_t)`). Now resolves the field's actual C type
  (`adt_field_type_for_app` against `rty`, falling back to the active ABI spec's
  result family when `rty` is not a clean concrete app) and bridges to it when it
  is a pointer; else keeps the int64 carrier cast. Clears
  `dot-parametric-fn-field-call`, `make-struct-parametric-fn-field-infer`,
  `stdlib-lens-record-field`.
- **`digest-hex` -- fixture-source inline-C** (`tests/fixtures/digest-hex/input.tur`).
  NOT an emit bug: the `: cstr` (-> `const char *`) function bodies hand-wrote
  `return (int64_t)(intptr_t)__TUR_CNAME_digest/...`. Inline-C is emitted verbatim,
  so the cast was wrong at the source. Changed to `return (const char *)(intptr_t)
  ...`. Runs correct (matches the FIPS/RFC SHA-256/MD5 vectors).

**Remaining 14 (report stays OPEN), all one deeper sub-problem:** the
`cps->direct` call ARG path (`atoms_csv_call_typed` / the nil-call arm in
`emit_cps_ir.c`, and closure-env capture field types) casts the arg to the
callee's param type as reported by `cps_call_param_ctype` / `binder_ctype_full`,
but that reports the **generic int64 carrier** even when the callee is actually
emitted as a concrete-pointer spec-clone, or is a runtime builtin whose signature
is not in the binding table. Remaining fixtures + their shape:

- `show-wrapper-helper-dispatch` (3), `show-collections-content-hamt` (3):
  cps->direct call to a resolved `<name>__spec__...` clone whose emitted param is
  a concrete pointer (`tur_adt_Vec__int *`) / `const char *`, but the call site
  casts the arg to the generic `int64_t` carrier. Fix needs
  `cps_call_param_ctype` to report the CLONE's concrete param C type when the call
  resolves to a mono/spec clone.
- `session-effects` (1): `spawn(__t4)` -- `spawn` is a runtime builtin with a
  `void *` param; its signature is not in the binding table, so
  `cps_call_param_ctype` returns NULL and the plain-int arg is passed bare into a
  `void *` param. Fix needs a known-signature table for such builtins (or a
  declared param type).
- `reactor-fibers-park-chan` (1): a closure ENV field (`void * chp`, a channel
  capture) passed to `chan_hysend(int64_t, ...)`. The env-capture field C type
  (`binder_ctype_full` on the captured var) is `void *` while the callee param is
  the int64 carrier -- a capture-slot vs callee-param mismatch inside the emitted
  `__fn_*` closure body.
- `schan-worker-pool` (6): mixed binder-init straddles in BOTH directions
  (`int64_t` binder initialized from a `void *` expression and vice-versa) around
  the scheduler/channel worker plumbing.

These are a genuinely harder, higher-regression-risk change (threading resolved
clone/builtin signatures into the cps->direct arg emitter) than the three landed
above, which is why they are left for a dedicated follow-up. The
`-Wno-error=int-conversion` / `-Wno-error=incompatible-pointer-types` stopgap in
`src/main.c` stays until this remainder is drained.

## Resolution (2026-07-21): tail fully drained + stopgap removed -- RESOLVED

The remaining tail is drained; the whole fixture tree now emits **0**
`-Wint-conversion` and **0** `-Wincompatible-pointer-types` hard errors under
Apple clang (`tur emit-c <fixture> | clang -std=c99 -Wall -c` over all ~1442
fixtures). Each of the 8 straddle fixtures compiles clean AND runs correct
against `expected.stdout`. The stopgap in `src/main.c` is **removed** (both
`cc`-invocation sites); only `-Wno-error=implicit-function-declaration` stays
(the separate, still-open `tur_hamt_hash_xxh64`-prototype concern). Full suite
(`bash tests/run.sh`, 12-min timeout): **2244 passed, 5 failed**, and all 5 are
the pre-existing macOS-toolchain issues this report already documented -- `pipe2`
static-vs-nonstatic in `hrt-stdlib-cont`, `OSByteOrder.h` "function definition
not allowed here" in the three `image-*` fixtures, and the
`vec-push-byvalue-aggregate-escapes-frame` stdout mismatch (none are
int-conversion, none are caused by this change).

Landed, each keyed to the shape it fixes:

- **Group A -- cloneable-frame call arg -> concrete-pointer param**
  (`src/compiler/emit_cps_ir.c`, new `cc_cast_for_param`). The resumed
  cloneable-frame arg was cast with `cc_cast_for_kind`, which maps every nominal
  ADT/opaque kind to the generic `(int64_t)` carrier -- straddling a callee whose
  actual emitted param is a concrete pointer (`read_hyh(tur_adt_H *)`).
  `cc_cast_for_param` prefers the callee's real param C type (from the FN type's
  `arg_full_types`) when it is a pointer, keeping the kind-based cast otherwise.
  Clears `cloneable-owning-carrier-handle-capture`,
  `cloneable-owning-consuming-carrier`, `-consuming-recursive`,
  `-consuming-ref-field`.
- **Group D -- cps->direct call: int64 carrier -> `void *` param**
  (`atoms_csv_call_typed`). The `param_is_voidp && arg_is_ptr` bare-pass fired on
  the SEMANTIC "value is pointer-like" flag even when the arg's C expression was
  an `int64_t` carrier (`spawn(__t4)` with `__t4` declared `int64_t`). Now the
  bare pass requires the arg's ACTUAL C type to be a pointer; an int64 carrier
  into a `void *` param bridges `(void *)(intptr_t)`. Clears `session-effects`,
  `session-mp-effects`.
- **Group B -- closure-env `void *` field -> int64 param**
  (`src/compiler/emit_expr.c`, spec-dispatch reverse-straddle bridge). The bridge
  that reinterprets a concrete-pointer arg into an int64 param excluded `void *`;
  a captured env field `void * chp` passed into `chan_hysend(int64_t, ...)` slipped
  through bare. Now a `void *` arg into an int64 param bridges too. Clears
  `reactor-fibers-park-chan`.
- **Group C -- `__ps_N` binder-init straddles, both directions**
  (`src/compiler/emit_expr.c`, let binder-init). The recorded-ctype consumers
  excluded `void *` (concrete-pointer-only) so a `void *` `__ps_N` temp into an
  `int64_t` binder and an `int64_t` temp into a `void *` binder both straddled.
  Added `init_val_recorded_voidp` / `init_val_recorded_i64` flags feeding the
  existing bridges. Clears `schan-worker-pool`.
- **stdlib source inline-C (the straddle the stopgap was masking)**
  (`stdlib/httpd.tur:1773`). `httpd-cors-own-str` returns `: cstr` but its
  inline-C body returned `(int64_t)(intptr_t)(cs ? strdup(cs) : ...)` -- the
  `digest-hex` shape: inline-C is emitted verbatim, so the cast was wrong at the
  source. Changed to `(const char *)(...)`. This surfaced only once the stopgap
  was removed (30 `httpd-*` fixtures `(load "stdlib/httpd.tur")`); it is exactly
  the "the stopgap hides real straddles" case this report warned about.
