---
title: M4c Path A landing — final state after suite-wide bridge audit
category: Codegen / ABI — monomorphization plan refinement
description: Captures the empirical landing state of M4c Path A (arg-side + return-side) and the suite-wide `TUR_M3_AUDIT=1` audit conducted afterward. Path A successfully retired the bridge from 2 of the 3 fixtures the original M3 finding identified, AND made dispatch on parameterized non-HKT instances (`Eq Tuple2`, etc.) go via per-instantiation specs with no carrier indirection. But a suite-wide audit shows the bridge is still load-bearing for 14 fixtures across ~82 total crossings — every single one of which traces to a stdlib helper or generic-relay shape that the M4 plan did not propose to retire. The bridge is now "essential, not residual."
---

# Final state after M4c Path A landed

## What Path A retired

Direct dispatch on a non-HKT parametric typeclass instance now emits a
per-instantiation spec with by-value param types and concrete return
type, called directly with no `(intptr_t)` cast and no bridge spill at
the dispatch site itself. Empirically confirmed on:

- `emit-abi-trace` (Eq Tuple2 dispatch): **zero crossings** under
  `TUR_M3_AUDIT=1`.
- `typeclass-return-dispatch-result-wrapped` (Dec / return-dispatch):
  **zero crossings**.

This is the first time a typeclass-instance method dispatch in this
codebase has produced a clean by-value call with no carrier
indirection at the receiver-dispatch or return-dispatch sites.

## What the suite-wide audit shows

```
$ for input in tests/fixtures/*/input.tur; do
    flags=""; fdir=$(dirname $input)
    [ -f "$fdir/flags" ] && flags=$(cat "$fdir/flags")
    TUR_M3_AUDIT=1 ./build/tur $flags emit-c "$input" 2>&1 | grep m3-audit | head
  done
```

14 fixtures still cross the bridge. Total crossings: ~82. Breakdown:

| Fixture | crossings | Pattern |
|---|---|---|
| `vec-eq-ascribed-multi` | 20 | Eq Vec → `vec-eq?` (inline-C, int64 carrier) |
| `vec-eq-ascribed` | 10 | same |
| `option-of-tvec-eq` | 8 | Eq Option(Vec int) → `option-eq?` + `vec-eq?` |
| `vec-of-tvec-eq` | 8 | nested Eq Vec |
| `result-of-typed-eq` | 4 | Eq Result(Vec int) → `vec-eq?` |
| `map-of-tvec-eq` | 6 | Eq Map → `map-eq?` |
| `set-of-tvec-eq` | 6 | Eq Set → `set-eq?` |
| `mutmap-eq` | 4 | Eq MutableMap → `mutmap-eq?` |
| `data-literal-typed-empty` | 6 | data-literal Vec construction |
| `tuplen-struct-param-passing` | 6 | tuple-struct param passing |
| `generic-relay-aggregate-result` | 1 | SChan generic relay |
| `serial-composite-instances` | 1 | serialization composite |
| `tuple-type-bracket-sugar` | 1 | type bracket sugar |
| `typeclass-method-parameterized-result-decode` | 1 | inline-C Dec body |

Every crossing traces to one of:

1. **A stdlib collection-Eq instance** whose body calls an inline-C
   helper (`vec-eq?`, `map-eq?`, `set-eq?`, `mutmap-eq?`, `option-eq?`,
   `list-eq?`) that fundamentally requires the int64 carrier ABI to
   iterate opaque heap memory. Path A specializes the instance method's
   PARAMS to by-value; the helper call inside the spec body spills back
   to int64 via the new general bridge site
   (`emit_expr.c:2549-2580`). **The bridge is doing its job here, not
   leaking** — the helpers genuinely need the int64 ABI to dereference
   `data : ptr<void>` and walk len entries.

2. **An inline-C-bodied instance method** (the
   `typeclass-method-parameterized-result-decode` residual). Existing
   carrier-skip gate at `emit_module.c:1182-1200` correctly keeps
   inline-C bodies on the carrier path — synthesizing wrappers around
   hand-rolled C is out of scope.

3. **Generic relays and ascription edge cases** (`SChan`,
   `tuplen-struct-param-passing`, `data-literal-typed-empty`). These
   are pre-existing bridge-essential sites that M4 was not chartered
   to retire.

## Probed alternative: stdlib Eq Cons rewrite is blocked on a language gap

The Eq Option rewrite that landed in commit `aca5bdea` (mirror of the
Tuple2/Pair/Result M4c-pre pattern) works because Option's fields are
all by-value (`is-some : bool`, `value : A`) — `(.is-some x)` and
`(.value x)` project directly.

`Eq Cons` follows a different shape: `(defstruct Cons [A] (head A)
(tail :int))`. The tail is an `:int` carrier (a raw pointer to the
next Cons cell, or 0 for nil). A direct-projection rewrite needs to
recursively walk through tails, which requires recovering a `(Cons A)`
view of an `:int` value:

```turmeric
(defn cons-list-eq? [A] [t1 : int t2 : int] [(Eq A)] : bool
  (if (= t1 0) (= t2 0)
      (if (= t2 0) false
          (let [c1 (:: t1 (Cons A))
                c2 (:: t2 (Cons A))]
            (and (eq? (.head c1) (.head c2))
                 (cons-list-eq? (.tail c1) (.tail c2)))))))
```

Probed empirically (`/tmp/cons_test.tur`): the `(:: t1 (Cons A))`
ascription elaborates cleanly but the emit treats `c1` as `int64_t`
and `.head c1` fails with `member reference base type 'int64_t' is not
a structure or union`. The carrier-bridge at `emit_expr.c:4166` (the
EX_ASCRIBE `carrier→concrete` path) DOES handle this case in
principle, but its `if` gate (`type_kind_is_aggregate(e->type.kind)
&& !type_uses_carrier_abi(e->type)`) requires the target to be a
non-carrier-ABI aggregate; `(Cons A)` with a TYVAR fails the
`!type_uses_carrier_abi` predicate because TY_APP is carrier-ABI in
the resolver.

To unblock the Cons rewrite either:
- The ascription bridge gate widens to fire when the resolved target
  is a concrete struct (post-substitution), or
- A new `(cast int (Cons A))` primitive emits the deref inline.

**Empirical resolution** (follow-up turn): The ascribe-bridge widening
AND the matching let-binding type widening shipped, both narrowly gated
to fire only when the inner expression past EX_ASCRIBE unwrap is a plain
TY_INT (EX_INT_LIT or non-carrier EX_VAR with TY_INT type) — NOT a
carrier-returning call. This distinguishes `(:: t (Cons int))` (raw
int handle → deref) from `(:: (vec-of) (Vec int))` (carrier relabel,
no deref). The inline pattern `(.head (:: t (Cons int)))` now emits
the deref directly and the let-bound pattern `(let [c (:: t (Cons
int))] (.head c))` declares the binding as `Cons__int c_NNN`,
matching the bridge's by-value output.

Probed Eq Cons rewrite end-to-end with this widening: the inline body
elaborates and the per-instantiation spec emit generates BOTH a
`Cons__int_Cons__int` spec (correct by-value signature) AND a
spurious `int64_t_int64_t` spec whose signature and body disagree
(int64 params but `(x).head` body). The clone-name mangling
(`emit_abi_clone_name` → `type_c_name` on TY_APP) inconsistently
returns int64 in one path and Cons__int in another, so the same
arg_types yield two different specs. **The Cons rewrite is still
blocked**, this time on a deeper clone-name/signature consistency
issue in the spec generation. The bridge-widening edits in this turn
nonetheless ship cleanly: suite stays at 172 FAIL baseline; the
widening fires correctly for the narrowly-gated case and is a no-op
elsewhere. The widening is shipped infrastructure that the eventual
Cons rewrite (once the spec-name issue lands) will consume.

## What "deleting the bridge" would actually require

Beyond M4 / Path A:

- **Rewrite every stdlib collection helper** in pure Turmeric. `vec-eq?`,
  `map-eq?`, `set-eq?`, `mutmap-eq?`, `option-eq?`, `list-eq?` —
  plus their `*-fmap` / `*-fold` counterparts. Each requires expressing
  opaque-memory iteration through some abstraction that doesn't
  bottom out in an inline-C `int64_t (...)` signature. That's a
  separate language-design effort.

- **Synthesize Turmeric wrappers around inline-C instance bodies**.
  For `(definstance Decode [int] (decode [v] ```c return tur_ok(v); ```))`,
  emit a Path A spec whose body wraps the inline-C call with a
  by-value-to-carrier or carrier-to-by-value transform. Non-trivial
  in the inline-C substitution path.

- **Audit and retire each generic-relay site** individually
  (`generic-relay-aggregate-result`, etc.). These tend to be deep in
  effect/concurrency machinery.

None of these are M4 work; they're each their own multi-session effort
that intersects with M4 but isn't unblocked by it.

## Recommended status update for `m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md`

Update from "deletion gated on M4" to "deletion gated on a broader
ABI-agnostic stdlib effort that M4 does not encompass." Path A's
landing is a major narrowing of the bridge's role — dispatch through
the bridge is gone for the per-instantiation case — but the bridge as
infrastructure remains essential. The M3 finding's "Validation when M4
lands" section should be amended to note that **the audit will show
zero crossings on the suite's clean path** only after the stdlib
collection-helper rewrite plus the inline-C-body synthesis land.

## Cost / benefit at this point

**Final suite: 170 FAIL (better than the 172 baseline; only diff is
the resolved `hamt-delete` transient).** Zero net regressions across
all of M4c's landing turns. Path A's mechanism is now mature and the
pattern is fully documented for future application (return-type
substitution, by-value-carrier-param sink bridge, instance-method
spec routing).

## What's worth doing next

1. **Apply Path A's mechanism to HKT classes when Plan M6/M7 lands.**
   The current gate `!is_hkt` keeps Functor/Monad/etc. on the carrier
   ABI; once M6/M7 designs the HKT dispatch story, Path A's machinery
   extends naturally.

2. **Audit fmap/bind dispatches** — those still go through the carrier
   per the HKT carve-out. The same pattern Path A applied to Eq could
   apply to Functor over Option / Result once the design pass picks
   the dispatch shape.

3. **Plan M5** (constrained-polymorphic dict typing) remains the
   adjacent piece. Path A's per-instantiation dicts haven't been built
   yet — the spec routes directly without going through a dict struct.
   M5 would build the dict structure and let constrained polymorphic
   bodies index it typed.

## Audit data (snapshotted)

See `history/m4-suite-wide-bridge-audit-2026-06-13.txt` for the
raw `[m3-audit]` lines per fixture.

## Related

- [m4c-path-a-cascades-into-stdlib-eq-instances.md](m4c-path-a-cascades-into-stdlib-eq-instances.md)
  — arg-side landing.
- [m4c-path-a-result-side-needs-return-dispatch-elab-hook.md](m4c-path-a-result-side-needs-return-dispatch-elab-hook.md)
  — return-side landing.
- [m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md](m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)
  — the original M3 deletion gate.
- [docs/upcoming/m4-typeclass-per-method-abi-plan.md](../upcoming/m4-typeclass-per-method-abi-plan.md)
- [docs/upcoming/end-to-end-monomorphization-plan.md](../upcoming/end-to-end-monomorphization-plan.md)
  §M5 / §M6 / §M7 — adjacent work.
