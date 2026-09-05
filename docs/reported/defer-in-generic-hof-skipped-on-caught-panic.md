# A `defer` inside a generic HOF does not fire on a caught panic unwind (compiled path)

**Severity: medium.** A silent skipped cleanup on `panic` + `catch-unwind`,
and a compiled/interpreter divergence. Not a crash and not a wrong VALUE from
the surviving code -- the missed side effect is the defer body itself
(a free, an undo, a log), so it surfaces as a leak or unrestored state rather
than a diagnostic.

**Status:** OPEN. Found 2026-09-05 while checking whether a `defer`-based
bracket could remove the `bt-scope` panic caveat (backtrackable-state guide).
Not introduced by that work -- the repro has no trail in it.

## What happens

A `defer` registered inside a function with a type parameter (`[A]`) is
skipped when the function's body panics and the panic is caught by an
enclosing `catch-unwind`. The same defer in a NON-generic function with an
otherwise identical body fires correctly.

```turmeric
(defn gen-hof [A] [^fat body : (fn [] A)] : A
  (do (defer (println "GEN defer fired")) (body)))
(defn mono-hof [^fat body : (fn [] int)] : int
  (do (defer (println "MONO defer fired")) (body)))

(defn main [] : int
  (catch-unwind (fn [] (mono-hof (fn [] (do (panic "x") 0)))))
  (println "--")
  (catch-unwind (fn [] (gen-hof  (fn [] (do (panic "y") 0)))))
  (println "done")
  0)
```

Compiled (`tur run`):

```
MONO defer fired
--
done                 <- "GEN defer fired" is MISSING
```

Interpreted (`tur --interpret`):

```
MONO defer fired
--
GEN defer fired      <- fires here
done
```

So the two execution paths disagree, and the compiled path is the one that
drops the cleanup.

## Why it matters for backtrackable state

`bt-scope` is `(defn bt-scope [A] [^fat body : (fn [] A)] ...)` -- a generic
HOF. Its documented panic caveat ("A panic inside `body` skips the undo") is
usually attributed to it using the explicit `bt-mark` / `bt-undo-to!` halves
rather than a `defer`. This report shows that rewriting it to a `defer` would
NOT fix the caveat on the compiled path: the defer would be skipped on exactly
the panic unwind it was meant to cover, and silently. The caveat is therefore
a symptom of this bug, not merely a design choice, and closing this is the
prerequisite for a panic-safe `bt-scope`.

The interpreter already runs the defer, so a fix should converge the compiled
path onto the interpreter's behaviour, not the other way round.

## Root cause -- not established

Likely in how a monomorphized/generic function's `defer` frames are registered
relative to the panic unwind boundary (the non-generic twin works, so the defer
machinery itself is fine). The `[A]` is the only difference between the two
functions above. A place to start: whether the generic body's defer-frame
registration is emitted on the specialized clone or lost when the `[A]` head is
lowered, and whether the unwind walks the clone's frame list.

## Scope -- who else pays this

Any generic higher-order (or plain generic) function that registers a `defer`
and can panic under a `catch-unwind`. The value-returning normal-exit path is
NOT affected (the defer fires on a normal return in both paths); it is
specifically the caught-panic unwind that skips it on the compiled side.
