# ICE: a `let` whose result is a NON-capturing lambda trips the repr-shadow guard

**Severity: medium.** Split out of
`docs/archive/httpd-mw-recover-unblocked-but-unwritten.md` (repro A) on
2026-08-21, when the other three blockers in that report were fixed and
`mw-recover` shipped. This one survives on its own.

## Repro

```turmeric
(defn wrap [] : int
  (let [_x 1]
    (fn [c : int] : int c)))
(defn main [] : int 0)
```

```
tur: internal error (ICE): a representation decision disagrees with repr_of at merge-temp.
  repr-shadow merge-temp result type=(fn [int] : int) want=fat-handle got=carrier-i64 cty=int64_t own=int64_t
```

## What narrows it

Three conditions, all necessary:

| Variant | Result |
| --- | --- |
| the repro above | **ICE** |
| lambda body uses `_x` (so it captures) | compiles |
| `(do ...)` instead of `(let ...)` | compiles |
| lambda returned directly, no wrapping form | compiles |

The return type of the enclosing `defn` is *not* a factor -- `: int` and
`: ptr<void>` both ICE. So the trigger is specifically **a `let` merge
temp whose value is a captureless lambda**: the lambda lowers to a bare
function pointer (`carrier-i64`), the merge temp was already decided as
`fat-handle`, and the shadow guard catches the disagreement.

## Why it is not just a guard question

Under `TUR_REPR_NO_SHADOW_ICE=1` the repro compiles, runs, and exits 0,
so for this shape the disagreement is benign. That is not a licence to
relax the guard. `docs/archive/let-bound-noncapturing-lambda-segfaults-as-fn-arg.md`
is the same family -- a bare pointer reaching a consumer that wanted a
fat handle -- and there it was **silent** and segfaulted. The guard is
reporting a real inconsistency.

## Fix direction

Make the value actually be a fat handle at the merge temp, rather than
teaching the guard to look away. The archived fix at the *argument*
boundary (`ensure_bare_fnptr_poly_shim`, a signature-keyed adapter that
wraps a bare function pointer into a `{shim, NULL}` fat pair) is the
shape to reuse: apply it when a `let`/merge temp's declared repr is
`fat-handle` and the incoming value is a captureless closure. The
alternative -- make `repr_of` agree that a captureless closure is a bare
pointer -- means auditing *every* consumer to handle one, which is the
larger change and the one that produced the archived segfault.

See `docs/archive/repr-decision-function-plan.md` for the defect family.

## Guides to update when fixed

None -- no documented surface promises this shape today.
