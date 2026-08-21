# Guide examples call `println` with 2 args and `when` with 2 body forms

**Severity: low-medium** (documented examples that do not compile). Found
2026-08-21 while updating `cli-args-guide.md` for
[args-api-int-erased-handles](../archive/args-api-int-erased-handles.md).

## Repro

```turmeric
(println "count:" n)
;; TUR-E0006: operator lookup failed for 'println': got 2 arg(s), first arg type cstr

(when (not (= args 0))
  (println (list-head args))
  (walk (list-tail args)))
;; stdlib/macros.tur:49: error: macro 'when' expects 2 arguments, got 3
```

## Root cause

- `println` is registered per argument TYPE, each entry arity **1..1**
  (`src/compiler/builtins.c:119-126`). There is no variadic or 2-argument
  form, and no `print` (no-newline) builtin either. A label + value needs
  `(println (str-concat "label: " v))` with `str-concat` / `int->str` from
  `stdlib/str-build.tur`, or two separate calls.
- `(defmacro when [test body] ...)` (`stdlib/macros.tur:49`) takes exactly one
  body form; two statements must be wrapped in `(do ...)`.

Two neighbours found in the same pass, both verified:

- **`cond`'s catch-all must be `:else`, not `true`.** The macro
  (`stdlib/macros.tur:40`) recurses to a bare `nil` when the clause list runs
  out, so a `true` final clause leaves `(if true <int> nil)` and the whole form
  fails with `if branches have mismatched types: then=int else=nil`. `:else` is
  special-cased to drop the tail.
- **`head` / `tail` are not bound in the compiled path** (only `list-head` /
  `list-tail` from `stdlib/list.tur` are), and the interpreter's `head` native
  yields the element as an `:int` -- so `(println (head *args*))` prints a
  POINTER under `--interpret` and does not compile at all under `tur run`. The
  working spelling is `(:: (list-head *args*) :cstr)`.

## Remaining sites

`docs/guides/cli-args-guide.md` was fully corrected (every example in it is now
compile-verified in both dialects, `head`/`tail`, `when`, `cond` and `println`
alike). Not swept:

| File | Sites |
| --- | --- |
| `docs/guides/frame-guide.md` | 12 (`(println "error:" e)` in match arms, `(println "schema ptr:" p)`) |
| `docs/guides/developing-spices-guide.md` | 1 (line 777, inside a `;;;` docstring example) |

`frame-guide.md` documents `tur-frame`, a spice in the sibling
`../turmeric-spices` checkout, so its example TYPES could not be verified in
this container -- only the `println` arity defect, which is independent of
them.

## Fix direction

For a cstr value: `(println (str-concat "error: " e))` (needs
`(load "stdlib/str-build.tur")`). For a value whose type is not known to be
cstr, two calls -- in a one-form position (a match arm) that is
`(do (println "error:") (println e))`, or neoteric `do(...)` in sweet-exp.

A doc lint would be cheap insurance: extract fenced ```turmeric blocks from
`docs/guides/*.md` and `tur check` the self-contained ones. Most snippets are
fragments, so the lint would need an opt-in marker -- but the whole-program
examples (the ones a reader actually copies) are exactly the ones that would
be covered.

## Guides to update when fixed

- docs/guides/frame-guide.md
- docs/guides/developing-spices-guide.md

## Resolution (2026-08-21, same day)

Swept. `docs/guides/cli-args-guide.md` was already fully corrected when this was
filed; the remaining 13 sites are now fixed too:

- `docs/guides/frame-guide.md` (12) -- the four `(println "error:" e)` match
  arms and the two pointer prints, in both dialects. They use the **two-call**
  form (`(do (println "error:") (println e))`, `do(println("...") println(e))`
  in sweet-exp) rather than `str-concat`, deliberately: `tur-frame` lives in the
  sibling `../turmeric-spices` checkout, so the error payload's TYPE could not
  be verified here, and two calls are correct whatever it is. A one-line note at
  the first site says why a label and a value are two calls.
- `docs/guides/developing-spices-guide.md` (1) -- the `;;;` docstring example.

`grep -rE '\(println "[^"]*" [^)]|println\("[^"]*" ' docs/guides/*.md` is now
empty across every guide.

The lint idea in the fix direction is NOT done: extracting fenced ```turmeric
blocks and `tur check`ing the self-contained ones is worth building, but it
needs an opt-in marker (most snippets are fragments) and is a bigger piece than
this sweep. The `no-check` fence marker some guides already carry
(`schema-guide.md` uses it) looks like the seed of exactly that convention.
