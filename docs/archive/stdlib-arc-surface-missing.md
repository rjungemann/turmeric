# Arc has a runtime but no stdlib/language surface

**Severity: low** (expressiveness hole). Found in the 2026-08-20 docs audit.
**Status: RESOLVED** -- `stdlib/arc.tur`.

## Repro

`src/runtime/arc.{c,h}` existed but there was no `arc*` defn anywhere in
`stdlib/`. `tests/fixtures/arc-basic` -- the only user in the tree --
hand-rolled the control block in inline C. threading-guide.md documented
`arc`/`arc-clone`/`arc-deref` as built-ins (corrected in the audit).

## The hand-rolled block had already drifted

Worth recording, because it is the argument for the module rather than a
by-product of it. The fixture's private block was

```c
struct { uint64_t strong; uint64_t weak; int64_t value; }
```

while the real `ArcControlBlock` (src/runtime/arc.h) is

```c
struct { uint64_t strong_count; uint64_t weak_count; void *value;
         RcDropFn drop_fn; TypeKind value_type_kind; }
```

-- different field names, `value` a pointer rather than an inline int64, and
two trailing fields the copy did not have. Nothing linked the two, so nothing
noticed. Same shape as the HttpdConn redeclaration defect
(docs/archive/history/httpd-conn-struct-consolidation-plan.md), one copy in.

## Resolution

`stdlib/arc.tur`, not auto-loaded. `Arc` and `ArcWeak` are separate
`defopaque` handles over `:ptr<void>` -- per CLAUDE.md's `:int` rule, and
because confusing a strong handle with a weak one should be a type error
rather than a runtime abort.

| | |
|---|---|
| `arc-new [v : int] : Arc` | allocate, strong count 1 |
| `arc-clone [^borrow Arc] : Arc` | add a strong reference |
| `arc-get [^borrow Arc] : int` | read the value (borrowed) |
| `arc-strong-count`, `arc-weak-count` | inspect (borrowed) |
| `arc-drop [Arc] : void` | release one strong reference |
| `arc-downgrade [^borrow Arc] : ArcWeak` | non-owning handle |
| `arc-upgrade [^borrow ArcWeak] : (Option Arc)` | strong ref if still alive |
| `arc-weak-drop [ArcWeak] : void` | release one weak reference |

`arc-weak-count` subtracts the runtime's +1 sentinel (held while any strong
reference lives, so the block is not freed under a live weak handle) and
reports the weak handles a caller actually holds.

### Two things that had to be got right

**The struct declaration is hoisted once.** The `ArcControlBlock` layout and
the `arc_*` prototypes are declared at file scope via `__tur_include__` in
`arc/autolink-hint`, not repeated in each inline-C body -- the whole point of
the module is to stop having two definitions of this struct, and repeating it
nine times inside itself would have been a worse version of the same bug.
Hoisted from the module rather than added to the codegen preamble on purpose:
only programs importing `arc` pay for it, and no `expected.c` snapshot moves.

**`arc-upgrade` builds its Option in Turmeric, not in inline C.** The first
draft returned `tur_some_ptr(cb)` from the inline-C body and leaked 16 bytes
per successful upgrade -- LeakSanitizer caught it on the new fixture, since
`run.sh` runs compiled binaries with leak detection on. A `some` constructed
inside inline C allocates a box the elaborator never sees and never releases.
Keeping inline-C down to a raw `arc-try-upgrade : bool` predicate and building
the Option in ordinary Turmeric hands ownership back to the compiler. This is
the same split `stdlib/env.tur` already uses for `env/get` over
`env/get-raw`.

## Tests

- `tests/fixtures/arc-basic` -- **migrated** off the hand-rolled block onto
  the module. The assertions are unchanged; only the implementation moved, so
  it now exercises the real runtime instead of a copy of it.
- `tests/fixtures/arc-weak-upgrade` -- the weak half: weak count excluding the
  sentinel, upgrade-while-alive returning a real strong reference (count goes
  to 2), upgrade-after-drop returning none rather than a dangling handle, and
  the final weak drop.

Both carry `requires.compiled`. `arc-basic` used to be skipped from
`run-turi.sh` *by accident* -- `fixture_has_inline_c` greps the fixture file
for a ```` ```c ```` fence and it had one. Moving the implementation into the
module is exactly what removed that fence, and the detector follows
`(load "...")` but not `(import ...)`, so the skip now has to be declared.

Suites: run.sh 2673 passed / 0 failed; run-turi.sh 1843 passed / 0 failed;
run-stdlib-checks.sh 35 passed / 0 failed.

## Guide updated

docs/guides/threading-guide.md's Arc section said "there is no auto-loaded
stdlib wrapper yet, so today Arc is reached through inline C over the control
block". It now shows the module, adds a weak-reference subsection, and states
the one place Arc is weaker than `rc<T>`: there is no cycle collector, so a
strong cycle leaks and must be broken with `arc-downgrade`.

Regenerated `stdlib/docstrings.tur` and `docs/api/`.
