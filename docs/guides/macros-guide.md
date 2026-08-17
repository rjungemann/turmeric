# Macros (`defmacro`)

Turmeric macros are compile-time functions from **syntax to syntax**.  A
`defmacro` runs during elaboration, receives its arguments as unevaluated
forms, and returns a form; the returned form then flows through the FULL
elaborator -- type checking, typeclass dispatch, effect rows, refinements,
borrow checking, everything.  Macro-generated code is not a second-class
citizen: it is checked exactly like code you wrote by hand, in both engines
(compiled and `--interpret`).

```turmeric
(defmacro twice [e] `(+ ~e ~e))

(twice 21)       ; expands to (+ 21 21) => 42
(twice "oops")   ; expands to (+ "oops" "oops") => TUR-E0006 type error
```

That second line is the load-bearing property: the expansion is wrong, and
the TYPE CHECKER says so.  When a diagnostic points into generated code, the
compiler appends a note naming the call you actually wrote:

```
error [TUR-E0006]: operator lookup failed for '+': ... first arg type cstr
note: in expansion of macro 'twice' -- the diagnostics above are inside
      code this call generated
```

Every core control-flow form you use daily -- `cond`, `when`, `for`,
`do-m`, the contract forms -- is a `defmacro` in `stdlib/macros.tur`; read
that file for a working style reference.

## Quasiquote, unquote, splice

| Syntax | Meaning |
|---|---|
| `` `form `` | quasiquote: build the form as a template |
| `~e` | unquote: substitute the value of `e` into the template |
| `~@e` | unquote-splicing: splice a list's elements into the surrounding form |

Splicing works into calls, vectors, and (via structural recursion) any
depth of the template.  A macro can also call **functions and other
macros** inside a splice expression, so a template can *generate* its
spliced sequence rather than merely forwarding one.

## Hygiene: deliberately manual

Turmeric macros are **unhygienic by design** (Common-Lisp style): a binding
introduced by a template captures a same-named binding at the call site.

```turmeric
(defmacro bad [e] `(let [tmp 99] (+ tmp ~e)))
(let [tmp 1] (bad tmp))    ; => 198, NOT 100: the template's tmp captured yours
```

The discipline is `gensym`: mint a fresh symbol for every binding a
template introduces.

```turmeric
(defmacro good [e]
  (let [t (gensym "tmp")]
    `(let [~t 99] (+ ~t ~e))))
(let [tmp 1] (good tmp))   ; => 100
```

Rule of thumb: any `let`, `fn` parameter, or loop variable your template
creates gets a `gensym`.  Capture is occasionally what you want (anaphoric
macros); when you rely on it, say so in the macro's docstring.

## `^syntax` parameters: receiving raw AST

By default a macro argument is substituted into the template as code.  A
parameter marked `^syntax` instead binds the raw, unevaluated form, so the
macro body can WALK it with the compile-time builtins:

```turmeric
(defmacro field-name [^syntax decl]
  `(println ~(symbol-name (first decl))))
```

`^syntax` composes with variadic rest params (`& ^syntax decls`), which is
the workhorse for "iterate over my arguments" macros.

## Recursion over arguments

A variadic macro recurses over its rest list with `first` / `rest`, using
**`empty?`** as the base case:

```turmeric
(defmacro sum-all [& ^syntax xs]
  (if (empty? xs)
    `0
    `(+ ~(first xs) (sum-all ~@(rest xs)))))

(sum-all 1 2 3 4 5)   ; => 15
```

Two traps, both of which end in "maximum macro expansion depth exceeded"
(the cap is 256):

- **`nil?` is not the empty-rest test.**  An empty rest is an EMPTY LIST
  form, not nil, so `(nil? xs)` is always false and the base case never
  fires.  Use `(empty? xs)`.
- **There is no compile-time arithmetic** (see Limitations).  A "count
  down from N" macro cannot terminate: `~(- n 1)` splices the unevaluated
  form `(- n 1)`, which never equals `0`.  Drive recursion by argument
  list STRUCTURE, never by counting.

## The compile-time builtin set

Inside a macro body (outside templates, and inside `~`/`~@` escapes) the
compile-time evaluator provides:

- list/vector: `first`, `rest`, `second`, `cons`, `list`, `vec`, `nil?`,
  `empty?`, `list?`, `vec?`
- symbols and names: `symbol-name`, `str->sym`, `str-append`, `dot-sym`
  (single-argument: `(dot-sym x)` makes the `.x` accessor symbol),
  `gensym`
- syntax inspection: `type-ann?`, `type-ann-inner` (peek at a `: T`
  annotation form)
- logic: `=` (literals and symbols), `not`, and `if`

Plus calls to other macros and to compile-time-evaluable functions inside
splices.  That is the whole set -- notably absent: arithmetic, string
comparison beyond `=`, and any type inspection (see Limitations).

## Generating names, and whole typed declarations

`symbol-name` + `str-append` + `str->sym` synthesize identifiers, and a
single macro invocation can emit SEVERAL top-level definitions by wrapping
them in `(do ...)` -- top-level `do` splices into the program:

```turmeric
(defmacro def-record [name T]
  `(do
     (defstruct ~(str->sym (str-append (symbol-name name) "Rec"))
       [val : ~T count : int])
     (defn ~(str->sym (str-append (symbol-name name) "-mk")) [v : ~T]
       : ~(str->sym (str-append (symbol-name name) "Rec"))
       (make-struct ~(str->sym (str-append (symbol-name name) "Rec")) v 1))))

(def-record Score :float)
;; defines struct ScoreRec [val : float count : int]
;; and (Score-mk 7.1) : ScoreRec
```

## Types in templates: full power in the write direction

Type positions are ordinary syntax at expansion time, so a macro can
interpolate ANY type -- simple, compound, or synthesized -- into any type
position, and the checker resolves it downstream:

```turmeric
;; a passed-in type token
(defmacro typed-id [T] `(fn [x : ~T] : ~T x))
((typed-id :int) 42)

;; compound types, one template -> several typed instantiations
(defmacro defvecfn [name T dflt]
  `(defn ~name [v : (Vec ~T)] : ~T
     (if (> (vec-len v) 0) (vec-get v 0) ~dflt)))
(defvecfn first-int :int 0)
(defvecfn first-str :cstr "none")

;; synthesized names resolve (and FAIL like hand-written types when wrong)
`(defn make-it [] : ~(str->sym (str-append (symbol-name prefix) "Bar")) ...)
```

This extends to ascriptions (`(:: e (Vec ~T))`), `defstruct` field types,
inline-C-adjacent signatures, and even `#row{...}` elements
(`#row{~T int}`).  A synthesized type that resolves to nothing is a real
elaboration error, not silent acceptance -- the checker genuinely
evaluates what you spliced.

The one thing a macro CANNOT do with types is the read direction: see
Limitations.

## Emitting inline-C

A template may contain an inline-C block; the enclosing `defn` form is
emitted like any other.  Remember the repo style rule: the closing
` ``` ` and its `)` stay on one line (` ```) `).

## Limitations, stated plainly

These are design boundaries, not bugs.  Each has a written rationale in
[row-types-followups-plan.md](../upcoming/row-types-followups-plan.md) (R3)
and the documents it cites.

- **No compile-time arithmetic.**  The evaluator's logic is `=`/`not`/`if`
  over forms; there is no `+`, `-`, or `<` at expansion time.  Repetition
  is driven by argument list structure (recurse with `first`/`rest`/
  `empty?`), never by a counter.  If a counting macro seems necessary,
  restate the input so the count is a LIST (`(m a b c)` rather than
  `(m 3 x)`).
- **Macros cannot ask types.**  There is no `type-of` at expansion time:
  a macro cannot branch on an argument's inferred type.  The compile-time
  value domain is forms and compile-time functions only -- no `Type` case.
  Type-directed behavior belongs one layer down, in typeclasses, which
  dispatch on types with full checking.  (This is R3 of the row-types
  follow-ups plan, deliberately unscheduled; the guide you are reading is
  the loop-closure for it.)
- **Rows are not values** at runtime or expansion time (R2 of the same
  plan): `#row{...}` interpolates into type positions but cannot be
  inspected, matched, or passed as data.
- **Hygiene is manual** (see above) -- intended, not pending.
- **Expansion depth is capped at 256.**  Hitting the cap almost always
  means a base case that never fires; the diagnostic's note lists the two
  usual causes.
- **`dot-sym` is single-argument** -- it builds a `.field` accessor
  symbol.  General name synthesis is `str->sym` + `str-append` +
  `symbol-name`.
- **`=` compares literals and symbols**, not arbitrary forms; deep
  structural comparison of two syntax trees needs hand-rolled recursion.

## Debugging expansions

- A diagnostic inside generated code carries the note
  `in expansion of macro '<name>' ...` pointing at the outermost call you
  wrote.  Inner nested-macro frames are deliberately silent -- one note
  per user-visible call.
- To see what a macro produces, expand it in a scratch file and inspect
  the emitted C with `tur emit-c` (generated defns appear under their
  synthesized names), or evaluate the expansion under
  `tur --interpret` for the fastest iteration loop.
- `maximum macro expansion depth exceeded` -> re-read "Recursion over
  arguments" above; the answer is nearly always `empty?` or the
  no-arithmetic rule.

## History

The macro system's sharp edges were filed and fixed as they were hit;
the paper trail is in `docs/archive/history/` (splice-into-vector,
unquote in type position, inline-C emission, multiple top-level forms,
compile-time calls in splices, `^syntax` parameters, and more).  If a
limitation you hit is not listed above, check there before assuming it is
by design.
