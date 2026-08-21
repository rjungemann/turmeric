# No value-carrying Mutex<T>/RwLock<T> or scoped with-lock helpers

**Severity: low** (ergonomics). Found in the 2026-08-20 docs audit.
**Status: RESOLVED for the scoped helpers.** The value-carrying `Mutex<T>` is
deliberately not done -- see "Scope" below.

## Repro

`grep -rn "with-lock" stdlib/` -> nothing. stdlib shipped only the raw
`mutex-*` / `rwlock-*` handles, so every caller hand-wrote the
`(mutex-lock m) ... (mutex-unlock m)` sandwich, where the release drifts away
from the acquire as the body grows.

## Resolution

Three macros, in the modules that own the handles rather than a new file:

- `stdlib/mutex.tur` -- `(with-lock m body ...)`
- `stdlib/rwlock.tur` -- `(with-read-lock rw body ...)`,
  `(with-write-lock rw body ...)`

Each acquires, runs the body, releases, and yields the last body form's value,
so the form composes as an expression rather than a statement sandwich.

### The lock expression is evaluated twice, and that is forced

`(with-lock m ...)` references `m` once to lock and once to unlock. The
obvious alternative -- bind it once inside the macro -- **does not compile**:
`Mutex` is `:linear`, so

```turmeric
(let [lk m] ...)   ; error [TUR-E0100]: linear value 'lk' dropped without being consumed
```

a `let` binding moves the handle, and the checker then rejects the scope for
dropping it unconsumed. Verified before settling on the double reference, not
assumed. Consequence, documented on all three: pass a **variable**, never a
call -- `(with-lock (get-mutex) ...)` would lock one mutex and unlock whatever
the second call returned.

`defer` was also tried as the release mechanism. It works, but it fires at
*enclosing scope* exit rather than at the end of the body, so
`(mutex-free m)` later in the same scope runs before the deferred unlock --
unlocking a destroyed mutex. The explicit `let`-and-return shape keeps the
release inside the form where it belongs.

### Not panic-safe, and said so

If the body panics, the lock stays held. Making the release unwind-safe needs
it on a `catch-unwind` path, which the caller can express where it matters;
silently pretending otherwise would be worse than the note.

## Scope -- the value-carrying `Mutex<T>` is not done

The report asked for two things: the scoped closures, and "optionally a
value-cell Mutex struct". Only the first is here. A real `Mutex<T>` is not a
wrapper but a *type* -- it has to make the guarded value unreachable except
through the guard, which is the entire safety property, and Turmeric has no
way to express that today (a `defstruct` field is readable without taking the
lock, so the type would look like protection and provide none). Shipping a
`Mutex<T>` whose payload is reachable without the lock would be worse than not
shipping one. That needs a linearity/borrow story for the guard first.

## Also fixed in passing

`rwlock-free` did `free(rw)` on a handle that lowers to `int64_t`, an
int-to-pointer conversion warning under the cc-warning ratchet. It surfaced
because the new fixture is the first thing to load `stdlib/rwlock.tur` on the
compiled path. Both the `destroy` and the `free` now cast through `intptr_t`.

## Tests

`tests/fixtures/with-lock-scoped` -- single-form and multi-form bodies (the
last form's value is returned, earlier ones still run), and a re-lock after
each release, which is what actually proves the unlock happened rather than
just that the body ran. Covers all three macros.

Suites: run.sh 2674 passed / 0 failed; run-turi.sh 1843 passed / 0 failed;
check-cc-warn-ratchet OK.

## Guide updated

docs/guides/threading-guide.md's Mutex section leads with `with-lock` and
keeps the raw pair for acquire/release in different scopes, with both caveats
(double evaluation and why binding once is impossible; no panic safety).
docs/guides/stm-guide.md mentions no lock helpers, so nothing to change there.

Regenerated `stdlib/docstrings.tur` and `docs/api/`.
