# closure-drop-glue graduation blockers: ~33 flag-on teardown bare-frees

> **RESOLVED (2026-07-22, R4-prep) -- 33 -> 3, all crashes fixed.** The four
> teardown clusters' bare-frees of headered handles were routed through
> `TUR_CLOSURE_DROP` / `__dk_reap_closure` (all gated, flag-off byte-identical):
> - catch-unwind / catch-panic-of thunk box (`emit_expr.c`) -- 16 fixtures.
> - effect/shift receiver reap `__dk_reap_ptr((intptr_t)arg)` (`emit_cps_ir.c`)
>   -> `__dk_reap_closure` -- 8 fixtures.
> - struct/ADT fn-field drop glue `drop_fnfields_<T>` (`emit_module.c`) -- 5
>   fixtures (also fixed `dot-receiver-first-call`).
>
> A fresh forced-on full suite is now **2261 passed, 3 failed**. The 3 remaining
> are NOT crashes and NOT bugs: `rc-auto-drop-closure-capture`,
> `rc-elision-negative-closure-capture`, `closure-env-free-with-owning-sibling`
> print an rc strong-count that legitimately INCREASES by 1-2 flag-on, because the
> drop-glue RETAINS an rc capture (the closure holds its own strong ref -- the
> sound, intended Model R semantics). Their `expected.stdout` is flag-off and
> cannot be changed now without breaking the flag-off suite; they are a
> graduation-time expected-output regen (alongside the 140 `expected.c`), not a
> blocker. See R4 in `docs/upcoming/closure-drop-glue-plan.md`. Original report
> below.

**Summary:** forcing `--enable=closure-drop-glue` ON corpus-wide and running the
full suite (snapshots moved aside so codegen churn cannot mask runtime failures)
yields **2231 passed, 33 failed**. Every failure is a teardown path that
bare-frees a now-headered fat handle -- an interior free of the past-header
pointer -- confirmed as `invalid pointer` / SIGABRT crashes (and, where leak
detection is on, LSan aborts). Same bug class as the httpd/reactor/DK-reap sites
already fixed, but in machinery never exercised flag-on before.

**Severity:** Medium -- these are pre-existing LATENT flag-on issues, not
regressions: the flag-off suite is 2264/0, and every opted-in closure-drop-glue
fixture passes. They surface only when the experiment is forced on. But they are
**graduation blockers**: R4 (making the header ABI default / deleting the flag)
cannot proceed until every fat-handle free routes through `TUR_CLOSURE_DROP`.

**Repro:** default `g_opt_closure_drop_glue = true` (`src/runtime/globals.c`),
rebuild, `tur --enable=closure-drop-glue build <fixture>` + run under ASan:

    panic-catch-unwind-basic       -> AddressSanitizer: attempting free on an
                                      interior pointer / "invalid pointer", SIGABRT
    capturing-closure-struct-field -> same

**Root cause -- four teardown clusters, each with a bare free of a headered
handle** (mirror the fix already applied to `httpd-async-free` /
`tur_reactor_release_box` / `__dk_reap_run`):

1. **catch-unwind / panic (~16):** `catch-unwind-{branch,byvalue}-result-return`,
   `catch-unwind-ok-val-extract`, `panic-catch-panic-of`,
   `panic-catch-unwind-{basic,captures,caught,defer,nested,signal}`,
   `panic-in-handler`, `panic-reset-clears`, `panic-with-catch-of`,
   `stackless-catch-unwind-{outer-catch,byref-aggregate-group-bail,byref-aggregate-reader-transitive-escape-bail}`.
   The catch-unwind box / defer / handler teardown frees a captured closure or
   caught-box payload with a bare free.
2. **effect / continuation / shift-resume (~8):** `cross-function-resume-via-effect`,
   `effect-cont-kv-sugar`, `effect-fn-payload`, `effect-fn-payload-capturing`,
   `multishot-effect-cont-kv-sugar`, `shift-crossfn-resume-{named-fn,nested-reset,works}`.
   Effect-payload closures / captured continuations freed without the header.
3. **fn-field / struct-closure drop (~5):** `capturing-closure-struct-field`,
   `defstruct-field-arrow`, `defstruct-fn-field-struct-cstr`, `fn-field-unboxed`,
   `local-struct-fnfield-drop`. The struct/ADT fn-field drop glue
   (`drop_fnfields_*` / `free(&local)` path) bare-frees the boxed fn-field handle.
4. **rc-drop-closure + misc (~4):** `rc-auto-drop-closure-capture`,
   `rc-elision-negative-closure-capture`, `closure-env-free-with-owning-sibling`,
   `dot-receiver-first-call`.

**Fix directions:** audit each cluster's free site (emitted codegen and/or
runtime C) and route it through `TUR_CLOSURE_DROP` (emitted, byte-identical
flag-off) or the runtime header-aware release (`tur_reactor_release_box` pattern,
gated on `tur_closure_headers_enabled`) when it is precompiled libturi. This is
the bulk of R4-prep; see `docs/upcoming/closure-drop-glue-plan.md` R4. Until it
lands, graduation stays gated and the experiment remains opt-in.

**Blast radius of the codegen churn (separate from the above):** all 140
`expected.c` snapshots change flag-on (the header preamble is emitted into every
program), so graduation also carries a 140-file coordinated snapshot regen.
