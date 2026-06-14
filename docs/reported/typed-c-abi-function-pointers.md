# No typed C-ABI function pointer in the type system

**Status:** Reported
**Severity:** Language gap. Blocks S1 fixes in
`docs/reported/spices-int-stand-in-audit-2026-06-14.md` (5 callback sites
across `tourist`, `httpd`, `rtmidi`, `osc`). Without it, callbacks passed
to external C libraries must be typed `:int` or wrapped in a
`defopaque Foo :ptr<void>` that names the role but does not enforce the
signature.
**Discovered:** 2026-06-14, while scoping the language pre-work for the
spice-wide `:int` audit fix-up.
**Surface:** Both compiler types (`src/compiler/types.h`) and user-visible
syntax (no `extern-fn` / `c-fn` / typed function-pointer literal).

---

## Summary

Turmeric's `(fn [A...] B)` arrow types model **closures**, not raw C
function pointers. A captureless closure happens to lower to a bare
`B(*)(A...)` at the ABI, but the type system does not distinguish
captureless from capturing -- both spell `(fn [A...] B)` -- and the lowering
diverges:

- **Captureless**: emitted as a bare function value (`EX_VAR` of `TY_FN`);
  thin call convention; the value IS the code pointer.
- **Capturing**: emitted as a heap-allocated fat box
  `{ int64_t thunk; captures... }` of boxed `TY_FN`; fat dispatch reads
  slot 0 for the thunk and passes the box as the implicit env.

These two representations are **incompatible at the C ABI**. A C library
expecting `void(*)(double, unsigned char*, size_t, void*)` cannot accept
a `(fn ...)` value at large -- only a captureless one, and only via an
explicit `:int`/`:ptr<void>` cast that the type checker silently accepts.
The captureless-vs-fat ABI divergence has already been a real bug:
`docs/archive/captureless-closure-not-boxed-at-fat-ptr-void-boundary.md`
documents the resulting crash when one side reads a bare code-pointer as
a fat box.

Net effect: **the type system has no surface for "this parameter is a
bare C function pointer of shape `R (*)(A...)`"**, and so every callback
sink either takes `:int` (the audit's S1 class) or `defopaque _ :ptr<void>`
(named but unenforced shape). Neither catches a wrong-shape callback at
compile time.

## Observed

### No `TY_CFNPTR` in the type kind enum

`src/compiler/types.h:76–174` defines the `TypeKind` enum:

- `TY_FN` -- function type (closure-bearing; carries `as.fn.boxed` to
  distinguish bare vs fat at lowering time but not at the *user*-visible
  type level).
- `TY_PTR_VOID` -- raw pointer (`ptr<T>` with optional pointee). Models
  data, not callable.
- No `TY_CFNPTR` / `TY_EXTERN_FN` / equivalent.

### `(fn ...)` lowers two different ways from the same type

- Captureless: `src/compiler/elab_fns.c:2798` emits a bare `EX_VAR` of
  `TY_FN`. Thin C call: `result = f(args...)`.
- Capturing: `src/compiler/elab_fns.c:2901` emits an `EX_CLOSURE` of boxed
  `TY_FN` (post-B-1) or `TY_PTR_VOID` (legacy). Fat dispatch:
  `result = ((thunk_t)box[0])(box, args...)`.

The audit-relevant consequence: a route handler typed as
`(fn [Request] Response)` could be either rep, and tourist's middleware /
dispatch code currently takes `handler : int` / `:ptr<void>` to dodge the
question. That dodge is exactly the S1 finding.

### `extern-c` does not give parameters a C-ABI fnptr type

`src/compiler/elab_fns.c:2754–2803` handles `extern-c` declarations -- they
import a C function so it can be **called** from Turmeric. They do not
provide a type form for "a value of pointer-to-extern-C-function of shape
X" that could be used as a parameter type to a function that takes a
callback.

### Closure-representation-unification (shipped 2026-06-03) did NOT close this gap

`docs/archive/history/closure-representation-unification-plan.md` and
`docs/archive/history/closure-first-class-type-plan.md` (B-0..B-4) made
closures a first-class type distinct from `:ptr<void>`. That removed the
"is `:ptr<void>` callable?" overload and the boxed-vs-bare crash class.
It did **not** introduce a typed C-ABI function pointer surface. The
language still cannot spell `void(*)(double, u8*, int, void*)` as a type.

### Today's workaround inheritance

The audit's 5 S1 sites all use `callback : int` (or `handler : ptr<void>`
in httpd). Each is documented in prose, enforced by nothing. Concrete
sites:

- `../turmeric-spices/spices/tourist/src/tourist/middleware.tur:58` --
  `use! [fn : int]`
- `../turmeric-spices/spices/tourist/src/tourist/dsl.tur:185,198,211,224,240`
  -- `get!`/`post!`/`put!`/`delete!`/`any!` route handlers
- `../turmeric-spices/spices/httpd/src/httpd/server.tur:511,528` --
  `server-start [handler : ptr<void>]`
- `../turmeric-spices/spices/rtmidi/src/rtmidi/in.tur:120` --
  `midi-in-set-callback [callback : int]`
- `../turmeric-spices/spices/osc/src/osc/server.tur:138` --
  `server-add-method [handler : int]`

## Expected

A way to spell a typed C-ABI function pointer that:

1. Is distinct from `(fn ...)` (so the type checker does not silently
   admit fat closures where bare pointers are required).
2. Has a concrete signature the checker enforces at call / pass sites.
3. Lowers to a bare `R (*)(A...)` with no implicit env.

Strawman surface syntax (pick one):

- **`(extern-fn [A...] B)`** -- type-position form mirroring `extern-c`,
  emitted as `B (*)(A...)`. Pairs with a constructor form (e.g.
  `(extern-fn-of name)`) that takes a Turmeric defn whose body is inline-C
  / a captureless closure and yields a value of that type.
- **`(c-fn [A...] B)`** -- shorter alias; same semantics.
- **`(:fn-ptr A... -> B)`** -- keyword-leaning variant.

Whichever spelling: the elaborator rejects mixing `(fn ...)` with
`(extern-fn ...)` without an explicit conversion (which can fail for
capturing closures).

## Proposed fix directions

### Today (no language change)

For each S1 site, the realistic options are:

- **Option A**: Replace `:int` with a `defopaque CallbackName :ptr<void>`
  newtype. Document the callback shape in the docstring. The newtype stops
  cross-callback confusion (you can't pass a `RouteHandler` where an
  `OscHandler` is wanted) but does NOT enforce arity / parameter types.
- **Option B**: Leave `:int`, add a prose docstring + a regression fixture
  that exercises the callback at runtime. Worst of both worlds; do not do
  this unless a downstream rebuild blocker forces it.

The audit's recommendation is Option A as the today-fix for the 5 S1
sites, with a documented intent to migrate to the language feature
described below once it lands.

### v2 language work

Add a type form that names a bare C-ABI function-pointer signature, and
a constructor form that converts a *provably* captureless Turmeric defn
(or an `extern-c` declaration) into a value of that type. The elaborator
rejects capturing closures at the conversion site with a clear error
pointing at the captures and suggesting `extern-c` + a static helper.

Touchpoints (best guesses; verify before implementing):

- `src/compiler/types.h` -- new `TY_CFNPTR { Type* ret; Type** args; int nargs; }`.
- `src/compiler/parse_types.c` (or wherever type expressions are parsed)
  -- new keyword form.
- `src/compiler/elab_fns.c:~2798` -- conversion site; ensure the captureless
  branch can be observed at the type level.
- `src/compiler/emit_*.c` -- emit as `R (*)(A...)`.

The migration of the 5 S1 audit sites becomes a one-line per-site signature
change once the form exists.

## Validation of a fix

- A new fixture passes a captureless defn as a `(c-fn [...] ...)` typed
  parameter and runs the C side without a crash.
- A new fixture passes a *capturing* defn at the same site and gets a
  compile error citing the captures.
- A new fixture passes a wrong-arity defn at the same site and gets a
  compile error citing the arity mismatch.
- After the migration of the 5 S1 sites, attempting to register a
  wrong-shape handler at a tourist `get!` / `midi-in-set-callback` / etc.
  fails at compile time, not at runtime.

## Cross-references

- `docs/reported/spices-int-stand-in-audit-2026-06-14.md` -- the audit
  that depends on this fix; 5 S1 sites listed there.
- `docs/reported/no-stdlib-result-builder-for-inline-c.md` -- companion
  language-pre-work finding (separate problem; same triggering audit).
- `docs/archive/history/closure-representation-unification-plan.md` and
  `docs/archive/history/closure-first-class-type-plan.md` -- the closure
  work that touched adjacent territory but did not close this gap.
- `docs/archive/captureless-closure-not-boxed-at-fat-ptr-void-boundary.md`
  -- prior bug from the same root cause (representations diverge at the
  ABI; the type system does not distinguish them).
- `CLAUDE.md` -- "No Lazy `:int` Stand-Ins -- STRICT RULE" (added
  2026-06-14) -- this finding is exactly the kind of language gap that
  rule expects to be surfaced rather than papered over.
