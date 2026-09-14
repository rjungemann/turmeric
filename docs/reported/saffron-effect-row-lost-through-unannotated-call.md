# A Saffron `handle` does not see an effect performed one call deeper

**Severity: high.** In typed Turmeric a `handle` catches an effect performed
by a callee of the handled expression. In a `#lang saffron` file it does not:
the clause is reported *unreachable* (TUR-W0033) and the program aborts with
`tur: unhandled effect (tag N)` at run time. Only a warning stands between the
two, and its text ("the body does not perform 'Ask'") states the opposite of
what the body does.

Found writing `docs/guides/introducing-saffron.md`; it is the reason that
guide's worked example performs in the function it hands to `handle`.

## Repro -- the same program, both dialects

```turmeric
#lang saffron
(defeffect Ask [] : int)
(defn g [] (perform (Ask)))
(defn f [] (+ (g) 1))
(defn main []
  (println (handle (f) (Ask [] k) (resume k 41)))
  0)
```

```
warning [TUR-W0033]: handler clause for 'Ask' is unreachable: the body does not perform 'Ask'
tur: unhandled effect (tag 2)
Aborted
```

Drop the `#lang` line and annotate the three signatures (`g`, `f`, `main` all
`: int`) and the identical program prints `42`.

## Root cause (suspected)

The effect row is carried on a function's type. An unannotated Saffron `defn`
gets the `any` default for its return, and `any` carries no row, so the row
computed for `g` does not reach `f`'s type and `f` presents as pure to the
`handle` that wraps it. That also explains the shape of the damage: the
*direct* `perform` case works, because there the row never has to survive a
call.

Worth checking against the CPS backend's own eviction pass -- `work` in a
one-level program is CPS-lowered fine, so this looks like elaboration, not
lowering.

## Why it matters more than the warning suggests

The warning fires on the handler *clause*, so a reader sees it as "this clause
is dead" and deletes the clause -- which is the opposite of the fix. A row
that cannot be inferred should keep the clause and either error at the
`perform` or fall back to a dynamic handler lookup, not quietly promise a
handler that will not be installed.
