# The generic-spec matrix only ever compiled with gcc

**Severity: medium.** Invalid C under clang and gcc 14. gcc 13 only warns, and
the matrix compared printed output, so every one of these cells "passed":

```
vecget/if/fn      incompatible integer to pointer conversion assigning to 'void *' from 'int64_t'
vecget/some/fn    ... passing 'int64_t' to parameter of type 'void *'
mapget/some/fn    ... assigning to 'int64_t' from 'void *'
mapget/lambda/fn  ... passing 'void *' to parameter of type 'int64_t'
thunk/ident/cstr  ... assigning to 'int64_t' from 'const char *'
thunk/pair/cstr   ... assigning to 'int64_t' from 'const char *'
```

Found 2026-10-01 when the type fuzzer, which is armed with clang, surfaced a
`-Wint-conversion` the matrix had never shown (the CPS twin is in
`cps-typed-pointer-into-carrier-slot.md`), and the matrix was rerun under
`CC=clang`. **RESOLVED 2026-10-01.**

## Mechanism

A `fn` element read by an inline-C container primitive (`vec-get`,
`map-get`) is the int64 word, while a spec spells a `fn` element as `void *`.
A cstr spec's result is `const char *`, while a CPS binder holds the int64
carrier. Six joins between the two had no cast:

- an `if` merge temp, declared at the resolved `void *`, took the word from
  each arm (the cast check read the unresolved `type_c_name(e->type)`, which
  is `int64_t`);
- a `match` arm's `void *` temp went into an `int64_t` merge temp, which was
  never recorded, so nothing could ask;
- `some`'s spec (`void *` parameter) took a recorded `int64_t` temp;
- a lambda literal's carrier slot skipped an argument whose type spells
  `int64_t`, although the value emitted was a `void *` temp;
- a CPS raw-let assigned a `const char *` temp into an `int64_t` binder.

## Fix

Each join now asks what the two sides were emitted as (the recorded local C
types) and casts word↔pointer through `intptr_t`:

- `if_arm_word_ptr_cast` / `if_merge_temp_ctype` for `if` arms;
- `emit_merge_assign_bridged` for the `match` arms, with the match temps now
  recorded;
- the argument chain's reverse rule (word into a pointer parameter);
- the lambda carrier slot's recorded-pointer rule;
- the CPS raw-let's pointer-into-word rule.

`tests/generic-spec-matrix.py` now compiles with clang whenever it finds one
(`CC` in the environment still wins), so the next such cell fails the matrix.

## Verified

- `tests/fixtures/generic-fn-element-word-pointer-crossings` (all six shapes),
  compiled under clang with no gcc warnings, and `--interpret`.
- The full matrix under clang: 0 failing.
