# `stm` inside a lambda that captures a TVar emits C that does not compile

**Severity: medium.** Filed 2026-09-30, found executing security-audit-plan
WP5 while probing whether `tvar/write` carries a region note. Pre-existing on
`main` @ 5fd23a65 (reproduced with the WP5 changes stashed).

## Repro

```turmeric
(defn run [f : (fn [] int)] : int (f))
(defn main [] : int
  (let [tv (tvar/new 0)]
    (println (run (fn [] : int (do (atomically (stm (tvar/write tv 5))) 1)))))
  0)
```

`tur build` fails in cc:

```
error: 'tv_5' undeclared (first use in this function)
    tur_tvar_write(tur_stm_current_tx(), (TVar*)tv_5, (void*)(intptr_t)...);
```

`tur check` passes. The same `(atomically (stm (tvar/write tv 5)))` at the top
of `main`'s body -- not inside a lambda -- builds and runs
(`tests/fixtures/stm-cas`).

## What it looks like

The `stm` body is lifted into its own C function, and `tvar/write`'s inline-C
body is spliced in with `tv` spelled as the enclosing lambda's binding name
(`tv_5`) rather than read out of the transaction thunk's env. So the capture
is lost at one of the two lifts -- the lambda's, or `stm`'s -- when they nest.
Not bisected further.

## Why it matters beyond STM

Every region bracket is a lambda (`(with-region (fn [] ...))`), so a
transaction over a TVar created outside the bracket cannot be written inside
one at all. That is also why WP5 could not probe `tvar/write`'s store hook with
a runtime fixture: the value reaches it through an explicit `(:: node ptr)`,
which is noted at the ascription, but the bracket around it does not compile.

## Fix directions

- Find which lift drops the capture: dump the elaborated `stm` form under a
  lambda and check whether `tv` appears in the thunk's free-variable set.
- Pin with a fixture that runs a transaction inside a lambda and inside a
  `with-region`.
