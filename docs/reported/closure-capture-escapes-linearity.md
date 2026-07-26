# A closure that consumes a captured `^linear` / `^unique` value escapes the checker

**Severity:** high (memory-unsafe -- a confirmed double-free, not a rejection gap)
**Status:** open
**Found by:** the "confirm no double-drop when a linear value is captured behind
an `rc`" open question in
[docs/upcoming/v1/gc-cycle-collection-followup-plan.md](../upcoming/v1/gc-cycle-collection-followup-plan.md)

## Summary

The linearity checker does not follow a linear or unique value through a closure
capture. Consuming a captured linear value inside a closure body does not mark
the outer binding consumed, so invoking the closure twice consumes it twice --
with no diagnostic.

```turmeric
(load "stdlib/serial.tur")
(defn main [] : int
  (let [b (bytes-alloc 8)]
    (let [f (fn [] : void (bytes-free b))]
      (f)
      (f)))                              ;; accepted -- no diagnostic
  0)
```

```
==3028==ERROR: AddressSanitizer: attempting double-free on 0x502000000010 in thread T0
SUMMARY: AddressSanitizer: double-free in free
```

The control is caught, which is what makes this a hole rather than a missing
feature -- the same two consumptions written without the closure are rejected:

```turmeric
(let [b (bytes-alloc 8)]
  (bytes-free b)
  (bytes-free b))
;; error [TUR-E0101]: linear value 'b' used after being consumed
```

## Scope

Measured on `Bytes` (a `:linear` struct with a real `bytes-free`), and on
`^unique` via `ref`:

| shape | result |
| --- | --- |
| `(bytes-free b)` twice, directly | **rejected** TUR-E0101 |
| `(rc/of b)` -- wrap a linear value | **rejected** TUR-E0103 |
| `(rc/of u)` -- wrap a unique binding | **rejected** TUR-E0202 |
| closure captures `b`, called twice | **accepted** -> double-free |
| closure captures `b`, aliased (`g = f`), each called once | **accepted** |
| closure captures a `^unique` value, called twice | **accepted** |
| closure capturing `b`, then `(rc/of closure)` | **accepted** |

Two of those deserve separate mention.

**The aliasing row is the nastiest.** Neither call site is locally suspicious --
`f` is called once and `g` is called once. Only the fact that `g` is `f` makes it
a double consume, and nothing in the checker connects them.

**`rc` is a vector, not the cause.** The question that prompted this was about
`rc`, and `rc` does make it worse -- `(rc/of closure)` is accepted even though
`(rc/of linear-value)` is rejected, so shared ownership of the closure gives you
a supported, ergonomic way to call it from several places. But the minimal repro
needs no `rc` at all. Fixing the `rc` surface alone would leave the bug intact.

## Root cause

`elab_fn` (`src/compiler/elab_fns.c:4398-4403`) collects the closure's free
variables for capture analysis and never consults their `CopyKind`:

```c
/* Phase 3: Capture analysis - collect free variables in the body */
uint32_t n_captures = 0;
Binding **captures = collect_free_vars(body, params, n_params,
                                       letrec_self_group, n_letrec_self_group,
                                       &n_captures);
```

Linear consume-state *is* tracked carefully elsewhere -- `if`/`cond`
(`elab_forms.c:2416-2623`) and `match` (`elab_structs.c:3356-3960`) snapshot,
branch, and merge it via `linear_state_snapshot_bindings` /
`linear_state_capture_current` / `linear_state_restore`. Closure bodies were
simply never wired into that discipline.

The concept already exists for the analogous case. `TUR_E0500` rejects a
`^multishot` handler that captures a `CK_UNIQUE` / `CK_LINEAR` binding
(`elab_effects.c:2045-2058`), using exactly the machinery this needs -- the same
`collect_free_vars` result, filtered on `CopyKind`:

```c
Binding **caps = collect_free_vars(cases[i].body, hparams, n_hparams, NULL, 0, &n_caps);
for (uint32_t ci = 0; ci < n_caps; ci++) {
    CopyKind ck = caps[ci]->type.copy_kind;
    if (ck == CK_UNIQUE || ck == CK_LINEAR) { /* TUR_E0500 */ }
}
```

So the gap is not a missing capability. A `^multishot` continuation is
definitionally invoked N times, so a blanket rejection is right there; an
ordinary closure is not, so the same check cannot simply be copied over.

## Fix direction

**A closure that consumes a captured linear value is itself linear.** That is the
standard treatment and it makes both bad shapes fall out of checks that already
exist, with no new diagnostic:

- `(f) (f)` becomes "linear value used after being consumed" -- TUR-E0101.
- `(rc/of f)` becomes "cannot wrap a linear value in rc<T>" -- TUR-E0103.
- Dropping the closure unused becomes "linear value dropped" -- TUR-E0100,
  which is also correct: the captured resource would never be released.

The same rule with `CK_UNIQUE` gives the unique case, via TUR-E0202.

**Consumes, not merely captures -- this distinction is the whole design.** A
closure that only *reads* a captured linear value is safe at any arity, and the
blanket E0500-style rule would reject a great deal of working code. Blast radius
measured across the fixture tree with a temporary probe in `elab_fn`:

```
fixtures affected: 73   total captures: 78
```

Most of those are almost certainly read-only captures -- the `httpd-*`
middleware closures dominate the list, and a per-request handler that consumed a
linear value would already be crashing on the second request. So the narrow rule
("the closure inherits linearity only if its body *consumes* the capture")
should shrink 73 to a handful, while the blanket rule would break all 73. Do not
implement the blanket version.

Determining "consumes" means asking, for each captured binding, whether the
closure body moves it into a consuming position -- the same question
`is_linear_consumed` already answers for straight-line code, evaluated against
the body with the capture in scope.

## Notes

- This was found while confirming an open question that was recorded as
  *unverified* rather than assumed safe. It was not safe.
- No fixture is added here. A fixture pinning the current behavior would have to
  assert a double-free, and one pinning the *correct* behavior would be a
  standing red. Both repros above are two-liners; reproduce with
  `TUR_CC_FLAGS="-fsanitize=address" tur build --runtime=source`.
- Related: the `^multishot` capture rule (TUR-E0500) is the nearest existing
  precedent and should stay as-is -- it is correct for its case.
