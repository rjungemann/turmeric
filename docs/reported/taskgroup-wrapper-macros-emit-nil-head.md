# `task-group-*` wrapper macros expand to an uncallable `nil` head

**Summary.** Every convenience macro in `stdlib/taskgroup.tur` --
`task-group-with`, `task-group-with-timeout`, `task-group-with-cancellation`,
and `task-group-async` -- is broken at macro-expansion time. Any program that
uses one fails to compile with:

```
error: expression in call head has type `nil`, which is not callable
```

**Severity.** Hard compile error (not a silent miscompile), but it renders the
entire documented structured-concurrency macro surface unusable. It is latent
because no fixture or test exercises these macros, so the suite stays green.
This finding was surfaced while typing the TaskGroup/TaskHandle handles for the
stdlib-opaque-handle-types plan; it is **pre-existing** and reproduces
unchanged on the parent commit.

## Minimal repro

```turmeric
(load "stdlib/taskgroup.tur")
(defn main [] : int
  (let [g (task-group-new)]
    (task-group-with g (task-group-done? g))
    (task-group-free g))
  0)
```

```
$ ./build/tur emit-c repro.tur
stdlib/taskgroup.tur:546:18: error: expression in call head has type `nil`, which is not callable
```

Observed: compile error pointing inside the macro definition.
Expected: the body runs, then `(task-group-wait g)` is called.

## Root cause

The macros build their expansion by *calling* `do` / `if` / `let` as ordinary
runtime functions on the macro arguments instead of *constructing* those forms
as data (e.g. via quasiquote or `cons`/`list` of the head symbol). For example
(`stdlib/taskgroup.tur:545`):

```turmeric
(defmacro task-group-with [group & body]
  (list (do body (task-group-wait group))))
```

At expansion time `(do body (task-group-wait group))` is evaluated: `do`
returns the value of its last subform, `(task-group-wait group)`, whose result
type is `nil`. The enclosing `(list ...)` then yields a one-element list whose
head is that `nil`, i.e. the expansion is `(nil)` -- a call with a `nil`
operator -- hence "call head has type `nil`, which is not callable".

The same shape appears in:

- `task-group-with` -- `stdlib/taskgroup.tur:545`
- `task-group-with-timeout` -- `stdlib/taskgroup.tur:552`
- `task-group-with-cancellation` -- `stdlib/taskgroup.tur:642`
- `task-group-async` -- `stdlib/taskgroup.tur:727`

`task-group-async` is the least wrong (its inner form is a real call,
`(task-group-spawn-async group async-thunk)`), but it is still wrapped in a
spurious extra `(list ...)`, so it expands to `((spawn-async ...))` -- a call
whose head is the *result* of `spawn-async` (a `ptr<void>`), not a callable.

## Proposed fix

Rewrite each macro to construct the form with quasiquote and splice the
variadic body, dropping the extra `(list ...)`. For example:

```turmeric
(defmacro task-group-with [group & body]
  `(do ~@body (task-group-wait ~group)))

(defmacro task-group-with-cancellation [group & body]
  `(if (task-group-should-exit? ~group)
     nil
     (do ~@body (task-group-wait ~group))))

(defmacro task-group-async [group async-thunk]
  `(task-group-spawn-async ~group ~async-thunk))
```

(Confirm the quasiquote/unquote-splicing surface syntax against other macros in
the tree, e.g. `stdlib/macros.tur`, before settling on the exact spelling.)

## How to validate a fix

1. Add a happy-path fixture under `tests/fixtures/` that drives each macro
   (e.g. `task-group-with` around a trivial body) and asserts the body runs and
   the group is awaited.
2. `./build/tur emit-c` on the repro above must succeed.
3. `bash tests/run.sh` stays green.

Until fixed, callers must use the non-macro API
(`task-group-spawn` / `-join` / `-wait` / `-cancel*` / `-free`) directly, which
works correctly.
