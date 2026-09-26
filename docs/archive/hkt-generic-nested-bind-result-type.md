# A two-binding `do-m` in a constrained generic does not compile

> **RESOLVED 2026-09-26.** See [Resolution](#resolution) at the end. The
> analysis below is the original filing.

**Severity:** medium -- a C type error at build time, no wrong answer;
`tur --interpret` prints the right answer. Found 2026-09-25 while writing the
SC8b step-5 fixture for typeclass-superclasses-plan; reproduces on the
compiler before that work with every constraint spelled out.

## Repro

```turmeric
(defn sum2 [^Monad M ^Applicative M] [a : (M int) b : (M int)] : (M int)
  (do-m x a
        y b
        (pure (+ x y))))

(defn main [] : int
  (println (match (sum2 (some 20) (some 22)) (Some v) v (None) -1))
  0)
```

```
error: incompatible types when assigning to type 'int64_t' from type
'tur_adt_Option__int'
```

A one-binding `do-m` (`(do-m x a (pure (* x 2)))`) and a single `bind` with a
lambda both compile. The `Result` instantiation fails the same way.

## Cause (probable)

`do-m` with two bindings is a `bind` whose continuation itself calls `bind`.
The inner `bind`'s result is typed as the by-value `(Option int)` monomorph in
the dictionary-passing spec (`sum2__dict_N__spec__...`) while the temp it is
assigned to is the `int64_t` carrier. The nested call is being given the
by-value result type that only a statically resolved call should get.

## Fix directions

Keep a nested dictionary-dispatched `bind` on the carrier inside a
dictionary-passing spec, or bridge its by-value result into the carrier temp.
Pin a two- and three-binding `do-m` at `Option` and `Result` under both
harnesses.

## Resolution

Two faults stacked, and a third shape turned up behind them.

- **The nested `bind` kept the representative instance.** Deciding whether a
  call inside a lambda dispatches on the constrained variable
  (`call_dispatched_constraint_class` in `src/compiler/elab_call.c`, mirrored by
  `emit_call_dict_env_dispatch_index` in `emit_core.c`) wanted a bare type
  variable receiver. A higher-kinded receiver is `(M int)`, an application
  headed by one, so the continuation captured no Monad dictionary and its
  `bind` was emitted against the representative instance. Both now key on the
  receiver's head, as the result test already did.
- **The capture assigned an aggregate to the carrier.** In a dict clone's spec,
  `b` is the by-value `tur_adt_Option__int`, while the continuation's shared
  env slot is the int64 carrier. The closure-env fill now heap-boxes a by-value
  aggregate captured into a carrier slot and notes the box's words for regions.
- **A constrained generic calling another one** did not compile or link (the
  case the forwarded-continuation fix noted). The inner call elaborated
  against the callee's carrier base, which a dict clone of the callee had
  since invalidated. `make_dict_clone` now redirects such a call to the
  callee's clone with the caller's dictionaries forwarded
  (`dict_clone_forward_generic_calls`). The clone returns the carrier, so the
  result is ascribed back to `(M int)` and unboxed wherever the active spec
  grounds it to a by-value type, including the caller's own tail, which the
  dict-clone return path re-boxes.

Pinned by `tests/fixtures/hkt-generic-nested-bind-result-type` (two- and
three-binding `do-m` at `Option` and `Result`, including the short-circuit
arms) and `tests/fixtures/hkt-generic-calls-generic` (nesting, a `let`, an
`if` arm, direct recursion and a forwarded continuation), under both harnesses.
