# closure-drop-glue graduation blockers: ~33 flag-on teardown bare-frees

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
