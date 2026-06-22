# Fix paper trail -- constrained generic instance-method dispatch (umbrella)

Resolved/archived 2026-06-22.

This was the umbrella report for constrained generic container typeclass
instances (`C [Vec]`/`[Option]`/`[Map]` with a `(C A)` element constraint). The
headline defect -- a class-method call on an element of the constrained tyvar
`A` dispatching to one fixed (int/first) instance instead of through the `(C A)`
constraint, silently returning wrong results or emitting `TUR_E0020` when no
`int` instance existed -- was fixed earlier (emit_reresolve_disp_type honouring
an ascription-to-constraint-tyvar receiver; the obj_is_abstract_tyvar fallback
to a carrier-compatible scalar representative). It is pinned by
`tests/fixtures/constrained-generic-instance-vec-element-dispatch/` (prints
`2`, `hello`, `F`), re-verified green on branch claude/keen-keller-acnewa.

The report carried two residual sub-cases. Both are now tracked as their own
dedicated OPEN reports, so the umbrella is archived (per the "residuals get
dedicated reports" convention) rather than left parked in docs/reported/:

1. Element call nested in a lambda-lifted closure (fold/accumulator shape) --
   `constrained-instance-element-dispatch-leaks-into-lifted-closures.md`.
2. Unascribed carrier-helper read collapses the element tyvar --
   `unascribed-carrier-helper-read-collapses-element-tyvar.md`.

No code change at archival time; this entry records the disposition.
