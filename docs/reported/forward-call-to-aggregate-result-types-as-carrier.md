# A forward call returning `(Result <imported opaque> E)` types as the int64 carrier

**Severity:** high -- it rejects correct, previously-compiling source at
`tur check`, and the shape (a wrapper written above its helpers) is ordinary
style. Three spices in `turmeric-spices` went red on it the day it landed.

**Status:** open, filed 2026-10-01. Reproduces on `main` at `8bb60d016`.

## One line

When a function's declared return type is `(Result T E)` and `T` is an opaque
imported from another module, a call to a helper **defined later in the same
module** has its type inferred as the int64 carrier rather than the aggregate,
so the enclosing function trips `TUR-E0709` ("declares return type ... but its
body returns int"). Moving the helpers above the caller makes the identical
code compile.

## Minimal repro

Two files, no spice checkout, no inline C.

`hx/core.tur`:

    (defmodule hx/core
      (export Handle)

    (defopaque Handle :int)
    )

`hx/use.tur`:

    (defmodule hx/use
      (import hx/core :refer [Handle])
      (export pick)

    ;; The caller is defined BEFORE the callees it names.
    (defn pick [c : bool] : (Result Handle cstr)
      (if c
        (good)
        (bad)))

    (defn good [] : (Result Handle cstr)
      (err "a"))

    (defn bad [] : (Result Handle cstr)
      (err "b"))
    )

```sh
tur check -I . hx/use.tur
# hx/use.tur:6:3: error [TUR-E0709]: function 'pick' declares return type
# '(Result Handle cstr)' but its body returns int -- an aggregate is a real C
# type (a struct, or a typed pointer to one), not the int64 carrier, so there
# is no representation these two share and nothing to bridge them
```

Move `good` and `bad` above `pick`: the same file passes. That is the whole
difference. Note that no `Handle` value is ever constructed -- both callees
return `err` -- so this is purely about how the forward call is typed.

## What is and is not required

Measured by adding one ingredient at a time against the same compiler. Each of
these compiles clean:

| Variant | Result |
| --- | --- |
| The repro above, callees moved above the caller | passes |
| Same shape, `Handle` declared in the *same* file instead of imported | passes |
| `(Result Box cstr)` with `Box` a same-file `defstruct`, caller first | passes |
| Same-file `defopaque :int`, with and without `:linear`, caller first | passes |
| Branches written as `(ok ...)` / `(err ...)` literals, not calls | passes |

So the trigger needs all three of:

1. the aggregate's payload type is **imported from another module**;
2. the caller is elaborated **before** the callee it names;
3. the callee's declared return type is the same aggregate `(Result T E)`.

`:linear` is **not** required -- `valkey`'s `Reply` is a plain
`(defopaque Reply :int)`. Nor is `if` -- `valkey`'s failing body is a single
tail call, not a branch.

## Where it bites in the tree

Three spices in `turmeric-spices`, all the same pattern, all green against a
compiler from the day before:

| Spice | Site | Order |
| --- | --- | --- |
| `secret` | `src/secret/hex.tur:198` `decode-then-wrap` | calls `wrap-decoded` (204), `fail-decoded` (210) |
| `secret` | `src/secret/kdf.tur:410` `hkdf-then-wrap` | calls `hkdf-with-cstrs`, defined below it |
| `valkey` | `src/valkey/cmd.tur:85` `cmd` | calls `__cmd-impl` (93) |
| `tourist-session-valkey` | -- | inherits `valkey/cmd.tur:85` through its dep |

`secret/hex.tur` is the clearest in-tree evidence that order is the variable:
in one file, with one declared return type, `secret->hex` (188) calls
`encode-then-wrap` (163) -- *above* it -- and passes, while `decode-then-wrap`
(198) calls two helpers *below* it and fails.

## Bisect

Same spice sources, two compilers:

| Compiler | `tur check` over `secret` + `valkey` |
| --- | --- |
| `a5edd6df8` (2026-09-30) | 0 failed |
| a build containing `8bb60d016` (PR #1007, merged 2026-10-01 17:44Z) | 3 files failed |

The window fits PR #1007's own subjects: `e4a09f50e` "no musttail across a
by-value aggregate; repr ratchet baseline" and `927529624` "P0 repr-confusion:
generic-spec matrix under clang -- six word/pointer joins".

**Not narrowed to a commit.** The decisive test is a build at `8bb60d016^1`
(`e7a33b96e`, `main` immediately before the merge). It was attempted and died
at link time with ENOSPC on the filing box -- a disk, not a code, result. Do
that build first: if `e7a33b96e` is clean, #1007 is the culprit and its commits
are a short list.

Corroborating timeline, independent of any local build: `turmeric-spices` CI
re-pins turmeric `main` once per run. Its last green `main` run
(36793999706, 2026-10-01T00:00Z, 98 jobs, 0 failures) predates the merge, and
the first run after it (36908677110) carries exactly these failures.

## Root cause

Not located in the compiler. What is known:

- The diagnostic comes from `src/compiler/elab_fns.c:10474-10483`, the
  `RET_CONFLICT_CARRIER_AGGREGATE` arm. That arm reports faithfully -- by the
  time it runs, the body's inferred type really is the carrier.
- The defect is therefore upstream of it, in the type given to a call whose
  callee has **not been elaborated yet**. For an aggregate return the
  placeholder appears to be the int64 carrier, which is precisely the
  representation the return comparison then rejects as unbridgeable.
- That the fault needs a cross-module payload type suggests the placeholder is
  only reached when the local pre-pass cannot already see a concrete aggregate
  layout for `T`.

## Fix directions

1. Give the forward-reference path the callee's **declared** return type.
   Signatures are known before bodies are elaborated; a call in a body should
   never fall back to the carrier for an aggregate return.
2. If a placeholder is genuinely unavoidable there, it must not be the carrier.
   Make it an unresolved marker the return comparison defers on, so the
   mismatch is re-decided once the callee is known instead of being settled
   against a stand-in.
3. Regression fixture: land the two-file repro as a positive fixture (it must
   compile), with a callee-first sibling, so a later change cannot half-fix
   this in one definition order only.

## Workaround

Reorder the helpers above their caller -- no semantic change, and it is what
the three spices need until this is fixed. It should not be landed as the
answer: it bakes in a definition-order requirement the language does not
otherwise have.
