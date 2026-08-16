# Dead base generic thunk chain still emits references to undefined base ctors

**Severity:** low -- the standard `tur build`/`tur run` pipeline (cc_flags
`-O2`) dead-strips the chain and links clean; the cliff is only reachable by
compiling `emit-c` output at `-O0` by hand, or by a (so far unobserved) live
carrier-path invocation of the base generic.
**Status:** OPEN.
**Found:** 2026-08-16, while verifying the `generic-closure-return-type-app`
fix (residue of that fix, filed the day it landed).

## Repro

```turmeric
(defn pure [A] [x : A] : (fn [] (Cons A))
  (fn [] (tcons x (tnil))))
(defn main [] : int
  (println (thead ((pure 5))))
  0)
```

- `tur run`: prints `5` (correct), but the cc step warns
  `implicit declaration of function 'ctor_Cons'` inside the emitted base
  `tcons`.
- `tur emit-c` then `cc -O0 out.c ... -lturi`: **undefined reference to
  `ctor_Cons`** at link.

## Root cause (pinned to the emission chain, not to a pass)

The `generic-closure-return-type-app` Defect B fix routes every invoke of the
lifted inner closure to a per-spec clone, so the shared BASE thunk is now dead
-- but it is still emitted. Its body references generic `tcons` unresolved,
which drags the base `tcons` into the emission set, whose body calls the base
`ctor_Cons` -- a symbol that is never defined for a parametric def. At `-O2`
the whole chain is unreferenced static functions and gets stripped; at `-O0`
it survives and the link fails.

Pre-fix, this same chain was the *live* path (that was the reported link
error). The fix moved the calls off it without suppressing its emission.

## Fix directions

- Suppress emission of the base lifted thunk when its result signature is the
  nonground shell (`result_kind == TY_APP/TY_ADT`, `result_full_type == NULL`)
  and per-spec clones were interned for it -- then also skip the base body
  scan so base `tcons`/`ctor_Cons` never enter the emission set. Care: the
  base OUTER generic fn's `EX_CLOSURE` construction references the base thunk
  symbol; either suppress that pair together or keep a forward decl so the
  compile stays clean and any genuinely live reference fails loudly at link
  (which it already would).
- Alternatively, register base-ctor forward declarations so at minimum the
  implicit-declaration warning goes away -- cosmetic only; the `-O0` cliff
  stays.

## Guide upkeep

Not a representation cell (no wrong value crosses a boundary -- the code is
dead); no `value-representations-guide.md` row. Noted in the archived
`generic-closure-return-type-app` report's status block as the fix's known
residue.
