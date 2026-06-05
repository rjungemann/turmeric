## Current (do now)

### The two anchors

- [ ] docs/upcoming/tur-signal-rebuild-plan.md -- the goal itself
- [+] docs/upcoming/stdlib-arrow-typeclass-reintroduction-plan.md -- mostly delivered (2026-06-05), but follow-ups
(arrow-class.tur merge-back, snapshot refresh) still open
- [+] docs/upcoming/stdlib-arrow-scaleback-plan.md -- superseded but the bare-combinator surface it locks in is what

### Hard prerequisites from still-in-flight-plan.md (these are tur-signal's own gating list)

- [+] "Language readiness for typed signal" -- G2 (poly-defn shared inner closure body) + G7 amber edge (>>> int-typed
in stdlib/arrow.tur)
- [+] "Signal Phase 0 spike" -- __sig_call_f macro + Option B bit-cast helper
- [+] "tur-signal spice broken build" -- Phase 0a (restore build, __arrow_call1 decision), 0b (closure-ABI cleanup), 0c
(:float sample migration)
- [-] "Closure representation unification" + "Typed closure invocation ABI" -- both feed G1/G7

### Closure / fn-first-class work feeding G1+G7

- [+] bare-fat-result-type-inference-plan.md -- unblocks :float-returning fat closures (G1)
- [+] fn-type-first-class-application-plan.md -- one predictable rule for callable :fn, needed for typed >>>
- [+] fn-first-class-stdlib-deworkaround-plan.md -- validates the above end-to-end
- [ ] return-type-dispatch-nullary-arrow-methods-plan.md -- required for Category.ident / ArrowZero.zeroArrow to resolve

### Reports on the critical path (docs/reported/)

- [+] function-arrow-not-instantiable-as-typeclass-head.md
- [+] fat-closure-dispatch-does-not-handle-struct-return.md
- [+] fat-closure-env-leak.md
- [-] fn-first-class-float-carrier-gap.md
- [-] ascribing-fat-closure-value-to-fn-type-double-shims.md
- [-] stale-pair-signals-typed-snapshot.md (signal-specific)
- [-] typeclass-methods-share-value-namespace-with-defns.md (resolved, but verify before signal lands)
- [-] result-param-order-blocks-functor-monad.md (resolved in 8596bce4 area -- confirm; needed by HKT-on-arrow combinators)

## Defer (post-arrow / post-signal)

### Stdlib evolution that's broader than arrows

- [ ] stdlib-hkt-consolidation-plan.md -- benefits from arrow landing but isn't a blocker
- [ ] stdlib-linearity-affinity-plan.md
- [ ] stdlib-refinement-collections-plan.md
- [ ] stdlib-session-typed-channels-plan.md
- [ ] stdlib-type-erasure-cleanup-plan.md (B6 already gated)
- [ ] range-gadt-typeclass-migration-plan.md

### Hygiene / tooling

- [-] reversible-name-mangling-plan.md
- [ ] Spaced-type annotation Phase 6/7 (CI enforcement)
- [ ] defmodule per-file scoping, "drop leading colons inside (fn ...) types" (both in still-in-flight)
- [ ] Stdlib opaque handle types tail (io/file-open annotation)
- [ ] HTTPD compression + tur/zlib spice

### Unrelated reports

- [ ] generic-struct-opaque-element-miscompile.md
- [ ] instance-method-returning-untyped-param-loses-result-type.md
- [ ] io-file-open-untyped-params-default-to-int.md
- [ ] load-not-idempotent-typeclass.md
- [ ] parameterized-defopaque.md
- [ ] taskgroup-wrapper-macros-emit-nil-head.md
- [ ] unsafe-block-capture-misses-ascription-vars.md

## One ambiguous case worth calling out

- [ ] return-type-dispatch-nullary-arrow-methods-plan.md is on the current side because it closes fix-direction #3 of the
- [ ] function-arrow-as-typeclass-head report -- but it's narrower than the typeclass reintroduction itself. If
- [ ] Category.ident / ArrowZero.zeroArrow aren't on tur-signal's actual call surface, you could defer it. Worth checking the signal plan's combinator list before committing time.


Phase 0c note: the SF-composition path (voice / voice-sf / poly-synth) uses
captured closures whose types the elaborator cannot infer through nested
let-bindings. They are gated until either a `^fat` let form supports full SF
types or the SF combinators are rewritten as direct helpers.
