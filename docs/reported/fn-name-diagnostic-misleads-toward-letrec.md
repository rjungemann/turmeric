# `(fn name [...] ...)` reports "parameter list must be a vector" instead of pointing at `letrec`

**Severity: low** -- diagnostics only. The rejection is correct (`fn` takes no
name binding); the message describes the wrong problem and sends the reader
looking for a syntax error in a parameter vector that is already well-formed.
No miscompilation, no wrong answers.

## Summary

Turmeric has no named-lambda form. A user arriving from Scheme, Racket, or
Common Lisp writes the recursive lambda the way those languages spell it:

```turmeric
(defn main [] : int
  (let [f (fn fact [n : int] : int (if (= n 0) 1 (* n (fact (- n 1)))))]
    (println (f 5))
    0))
```

and gets:

```
f1.tur:2:15: error: fn: parameter list must be a vector [name1 name2 ...]
1 | (defn main [] : int
2 |   (let [f (fn fact [n : int] : int (if (= n 0) 1 (* n (fact (- n 1)))))]
  |               ^^^^
```

The caret is on `fact`, and the message asserts that a parameter list must be a
vector. But `[n : int]` **is** a vector, and it is sitting right there one token
later. Nothing in the diagnostic says "`fn` does not take a name", so the
obvious reading is that the parameter vector is malformed -- and the reader goes
hunting for a bracket problem that does not exist.

The three spellings all produce the same message, each caret-ing whatever symbol
sits in the name slot:

```turmeric
(fn helper [] : int 1)     ; caret on `helper`
(λ named [] : int 1)       ; caret on `named`
(fn fact [n : int] ...)    ; caret on `fact`
```

## Why this is only a diagnostic bug

**The capability is not missing** -- only this surface syntax is. Recursion in
anonymous-function position works two documented ways, both verified to print
`120`:

```turmeric
;; letrec
(letrec [fact (fn [n : int] : int (if (= n 0) 1 (* n (fact (- n 1)))))]
  (println (fact 5)))

;; named let
(let loop [n 5 acc 1]
  (if (= n 0) (println acc) (loop (- n 1) (* acc n))))
```

Both are real: `elab_letrec` (`src/compiler/elab_forms.c:1972`) and the
named-let desugaring into `letrec` (`src/compiler/elab_forms.c:308`).

So this is not an expressiveness hole and does not warrant a new form. Plain
`let` also correctly refuses self-reference --
`error: unknown function or operator 'fact'`, caret on the use -- which is
proper non-recursive `let` scoping and reads fine as-is. The only thing wrong
anywhere in this area is the wording and caret placement above.

## Root cause

`fn` elaboration takes the form immediately after the `fn` head and requires it
to be `F_VEC`. When that slot holds an `F_SYM` (the name a Scheme user wrote),
the not-a-vector branch fires and reports the generic parameter-list error
against that symbol. The parameter vector one position further along is never
examined, so nothing notices that the form is well-formed apart from an extra
leading name.

The check is on the `fn`/`λ` path in `src/compiler/elab_fns.c` (the arm sharing
the `"defn"` / `"fn"` handling noted at `elab_fns.c:4194`); `fn` and `λ` are
both routed there via the special-form table at `src/compiler/elab_call.c:276`.

## Fix direction

Special-case the shape "the slot after `fn` is a symbol **and** the slot after
*that* is a vector" -- an unmistakable named-lambda attempt, not a random
malformed parameter list -- and emit a message that names the real problem and
the real remedy:

```
error: fn does not take a name; Turmeric has no named-lambda form
  |   (let [f (fn fact [n : int] : int ...)]
  |               ^^^^ remove this name
  = help: for a self-recursive anonymous function, use letrec:
            (letrec [fact (fn [n : int] : int ... (fact ...))] ...)
  = help: for a recursive loop, use a named let:
            (let fact [n 5] ... (fact ...))
```

Keep the existing generic message for the case where the post-`fn` slot is
neither a vector nor a symbol-followed-by-vector, since there the parameter list
really is the problem.

Worth pointing the same detection at `λ`, which shares the path and so gets the
fix for free.

## Suggested coverage

An `errors/` fixture with `expected.diag` asserting the new message for
`(fn fact [n : int] : int ...)`, plus a happy-path fixture pairing it with the
`letrec` spelling the help text recommends, so the suggestion stays true if
`letrec` ever moves.

## Provenance

Filed from investigating the assertion "No named-lambda recursion in Turmeric."
That assertion is **false as a capability claim** -- `letrec` and named `let`
both work, verified above -- and true only about the `(fn name ...)` spelling.
This diagnostic is the entire residue: the reason someone could hit that wall
and conclude recursion was unavailable is that the error message never mentions
`letrec`.
