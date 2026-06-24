# `(. obj field args...)` misroutes struct-field access through typeclass dispatch

**Severity:** medium (blocks the Clojure-style `(. obj method args)` ergonomics
for any struct whose field is function-typed; user-facing diagnostic is also
malformed)

**Status:** RESOLVED (routing + diagnostic + result-typing). Reproduced on a
`Lens`/`Person` toy at the REPL; produced a diagnostic that named no method.

## Resolution

Fixed in `src/compiler/elab_typeclasses.c` (`elab_method_call`):

1. **Dispatch misroute (#1)** -- the bare `.` head form `(. obj field args...)`
   was never desugared, so the head symbol `.` yielded an empty method name and
   fell straight into typeclass dispatch. Added a receiver-first desugaring at
   the top of `elab_method_call`: `(. obj field args...)` -> `(.field obj args...)`,
   after which the existing joined-form paths (struct field read, function-typed
   field call-through, typeclass dispatch) all apply uniformly.
2. **Empty name in diagnostic (#2)** -- added an explicit guard that rejects an
   empty method name with a clear message before it can reach the
   "no typeclass method found for ''" formatter.
3. **Call-through for function-typed fields (#3)** -- the capability-field
   call-through already built the indirect call `((.field obj) args)`, but typed
   its result as `:int`, so `(println (.get b p))` mis-dispatched the `cstr`
   result through the int instance and printed the pointer as a raw integer. The
   call is now typed by the field's declared return type (when concrete; bare
   `fn` fields keep the int64 carrier to avoid emitting a `void` temp).
   Semantics: `.field` auto-applies trailing args for function-typed fields,
   matching Clojure's `(. obj method args)`.

Regression fixture: `tests/fixtures/dot-receiver-first-call`.

**Carved out:** the *parametric* `Lens`/`Person` repro below still fails to
compile, but for an unrelated, pre-existing reason (a parametric struct stores
its `(fn [S] A)` field via the `int64_t` carrier, and the call site passes a
concrete struct value without bridging). That is independent of dot routing --
it reproduces identically with the joined `(.get l p)` form -- and is tracked
separately in
`docs/reported/parametric-struct-fn-field-call-passes-concrete-arg-to-carrier-ptr.md`.

## Summary

The reader/elaborator desugaring `(. obj field args...)` -> `(.field obj args...)`
(Clojure-style method-call sugar) does not behave correctly when `field` is a
struct field of `obj`'s static type. Instead of resolving `.field` as a struct
field accessor, the elaborator drives the lookup through the typeclass
method-dispatch path, fails to find a method, and emits a diagnostic with an
empty method name.

Three distinct bugs are visible in one repro:

1. **Dispatch misroute** -- `.field` on a value whose static type is a struct
   carrying that field should resolve to the struct field accessor, not a
   typeclass method lookup.
2. **Empty name in diagnostic** -- even on the typeclass-not-found path the
   method name is dropped before the formatter sees it, so the user gets
   `no typeclass method found for ''` instead of `... for 'get'`.
3. **No call-through for function-typed fields** -- once #1 is fixed,
   `(.get l p)` still needs to elaborate as `((.get l) p)` because `get` is a
   *field of function type*, not a method. A naive accessor will reject the
   extra argument as an arity error. Decide the semantics before fixing #1.

## Repro

```turmeric
(defstruct Lens [S A]
  (get (fn [S] A))
  (put (fn [S A] S)))

(defstruct Person :copy
  [name : cstr age : int])

(defn name-get [p : Person] : cstr
  (.name p))

(defn name-put [p : Person n : cstr] : Person
  (make-struct Person n (. p age)))

(let [p (make-struct Person "Bob" 40)
      l (make-struct Lens name-get name-put)]
  (. l get p))
```

Diagnostic produced:

```
<eval>:18:3: error: no typeclass method found for ''
15 | (do (define l (make-struct Lens name-get name-put)))
16 | (let [p (make-struct Person "Bob" 40)
17 |       l (make-struct Lens name-get name-put)]
18 |   (. l get p))
```

Expected: `(. l get p)` desugars to `(.get l p)`, which elaborates to
`((.get l) p)` -- fetch the `get` field of `l` (type `(fn [Person] cstr)`),
then apply it to `p`, yielding `"Bob"`.

## Root cause (direction)

The `(. obj field args...)` lowering appears to produce a method-call form that
is dispatched via the typeclass machinery first, with struct-field access only
as a fallback (or not at all). For a receiver whose static type is a struct
carrying `field`, the field accessor should win unambiguously.

The empty `''` in the error indicates the method-name slot is lost between the
desugaring step and the diagnostic builder -- likely the leading `.` is
stripped to an empty symbol, or the error is constructed before the name is
substituted in.

The third issue is a semantic-design question rather than a localized bug: if
`.field` is strictly a unary accessor, then `(. obj field args...)` cannot just
desugar to `(.field obj args...)` -- it must desugar to `((.field obj) args...)`
when `field` is function-typed, or the call-through has to be folded into the
`.` form's own elaboration.

## Fix directions

- In the elaborator for `(. obj field args...)` / `(.field obj args...)`:
  if `obj`'s resolved type is a struct with a field named `field`, lower
  directly to a struct-field-access node and, when extra args are present,
  wrap in an application: `((struct-field obj field) args...)`. Only fall
  through to typeclass method dispatch when no such field exists.
- In the typeclass-not-found diagnostic path: thread the method name into the
  error payload (or have the formatter pull it from the call node) so the
  printed name is never empty. Add an assertion that the name is non-empty
  at the point the diagnostic is constructed, so this regresses loudly rather
  than silently.
- Decide and document whether `.field` auto-applies trailing args for
  function-typed fields, or whether the user must spell `((.field obj) args)`
  explicitly. Clojure's `(. obj method args)` *is* the call, so matching that
  expectation means auto-applying; if the language prefers the explicit form,
  the `(. ...)` sugar should be restricted to the single-field-access case
  and reject extra args with a clear message.
