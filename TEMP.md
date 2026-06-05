## Current (do now)

### The two anchors

- [ ] docs/upcoming/tur-signal-rebuild-plan.md -- the goal itself
- [+] docs/upcoming/stdlib-arrow-typeclass-reintroduction-plan.md -- mostly delivered (2026-06-05), but follow-ups
(arrow-class.tur merge-back, snapshot refresh) still open
- [+] docs/upcoming/stdlib-arrow-scaleback-plan.md -- superseded but the bare-combinator surface it locks in is what
- [-] docs/upcoming/category-arrowzero-implementation-plan.md -- resolved-by-audit (2026-06-05, T0 outcome (c)): tur-signal Tier 1 calls neither ident nor zeroArrow, so no Category/ArrowZero [(->)] surface ships. Sequel-ready if a real consumer appears. See docs/reported/category-arrowzero-resolved-by-audit.md

## Defer (post-arrow / post-signal)

### Stdlib evolution that's broader than arrows

- [+] stdlib-hkt-consolidation-plan.md -- benefits from arrow landing but isn't a blocker
- [+] stdlib-linearity-affinity-plan.md
- [+] stdlib-refinement-collections-plan.md
- [+] stdlib-session-typed-channels-plan.md
- [+] stdlib-type-erasure-cleanup-plan.md (B6 already gated)
- [-] range-gadt-typeclass-migration-plan.md

### Hygiene / tooling

- [+] reversible-name-mangling-plan.md
- [ ] docs/upcoming/still-in-flight-plan.md - Spaced-type annotation Phase 6/7 (CI enforcement)
- [ ] docs/upcoming/still-in-flight-plan.md - defmodule per-file scoping (still-in-flight)
- [-] docs/upcoming/still-in-flight-plan.md - "drop leading colons inside (fn ...) types" (still-in-flight)
- [+] Stdlib opaque handle types tail (io/file-open annotation)
- [+] HTTPD compression + tur/zlib spice
