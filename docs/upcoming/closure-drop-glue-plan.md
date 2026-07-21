# Closure env drop glue -- freeing captured fat-closure environments

> # ⛔ ACTIVE / BLOCKING -- BUILD THIS NOW. DO NOT DEFER. DO NOT REWORD.
>
> This is not deferred, not "until later," not "the next slice," not a
> "quick win to bank," and not optional. It is the current front of the
> track and **all other work is blocked until the walk-glue lands and the
> leak-suppressed fixtures below are valgrind-clean.**
>
> **If you open this file, your job is to write CODE, not prose.** Editing,
> re-summarizing, re-scoping, or appending another "progress note" to this
> plan is **explicitly NOT progress** and does not count as picking it up.
> The plan has been analyzed to death; the load-bearing core
> (`drop_glue_env_N` + capture-time clone) is still at ZERO lines in `src/`.
> Verify that yourself: `grep -rn drop_glue_env src/` -> no hits.
>
> **Definition of done (the only thing that counts):**
> 1. Capture-time retain/clone for owning captures **and** the
>    `drop_glue_env_N` walk-glue land together as one atomic unit (see
>    "Implementation findings" -- either alone is unsound).
> 2. `free-lift-bind` and `unsafe-closure-capture` are valgrind/LSan-clean
>    and their `requires.no-leak-check` markers are DELETED.
> 3. Then `hkt-stdlib-parser-instances` and `cps-backend-fn-param` (the
>    escaping/colored cases) via S2 Model U + CPS admission, markers deleted.
>
> "It's too much work" / "it can't be safely chunked" is a description of
> THIS task, not a reason to hand it back. The hazard (double-free) is real
> and is exactly why 1a+1b are atomic -- build them atomic, prove it under
> valgrind, do not split them to dodge the hazard. Do not return until a
> real fixture flipped clean. This directive overrides the general
> "nothing blocks the track" posture in CLAUDE.md **for this work only** --
> by explicit owner instruction (2026-07-21).

**Status:** S1 and S2/Model U LANDED to their achievable extent -- every
value-closure, HOF-arg, and stored-in-a-GENERATED-holder (struct/ADT/`Parser`)
escaping closure is now freed, and all eight leak-check opt-outs the feature was
gating are dropped (suite 2249/0; see the 2026-07-21b/c notes). The ONE residual
is the **httpd/reactor server-closure family** (`httpd-async-mw-compose`,
`httpd-mw-*`, ...), which stays on `requires.no-leak-check`: its handler is stored
in an OPAQUE hand-written-C holder and reaches it type-erased through
`compose-middleware`'s `:int` return, so no static `drop_glue_env_N` can be
selected and Model U's generated-holder drop cannot reach it. Eliminating it
needs **Model R** (a runtime drop-glue pointer carried on the fat env), which this
plan deliberately DEFERS (high ABI cost; the captured strings are already a
documented process-lifetime pattern, `stdlib/httpd.tur:1762`). The Model R sketch
below is ready to execute if/when that family is prioritized. Prepared from
`docs/reported/escaping-fat-closure-env-leak.md` and the B2 residuals in
`cps-runtime-finish-plan.md` (Progress-log PD).

> **Progress note (2026-07-21g) -- Model R type-honesty (a): the `^fat`
> nested-closure walk LANDED (move-gated, sound). The middleware
> `(fn [next] (fn [conn] ...))` shape now frees its whole chain.** Three parts,
> all flag-gated:
>
> - **Identify (type-honesty).** A `^fat` capture is the erased int64 carrier in
>   the env FIELD, but the capture BINDING keeps `is_fat` (it propagates through
>   the `(let [_n next])` re-bind, elab_forms.c:878). So `cap->is_fat` distinguishes
>   an owned fat closure handle from a scalar int -- no new type needed, the signal
>   already survives. (The generalized fix -- carrying `^fat` as an owning-closure
>   TYPE rather than a flag-on-int -- is still worthwhile but not required for this.)
> - **Move (soundness).** Capturing a `^fat` handle now MARKS the source consumed
>   (`binding_mark_moved`, elab_fns.c), mirroring Model U's store-into-fn-field
>   move.  A SECOND capture of the same handle -- which would double-free -- is a
>   compile-time use-after-move (TUR-E0005), so the env is the handle's sole owner.
> - **Walk.** `drop_glue_<env>` releases each `is_fat` capture via TUR_CLOSURE_DROP
>   (uniform across representations thanks to 2026-07-21f), excluding the letrec
>   self-capture.  Releasing the outer handle frees the whole chain.
>
> Fixtures: `closure-drop-glue-fat-capture` (a `wrap`/`base` chain freed through
> the outer handle -- leak-clean, no double-free) and
> `errors/closure-drop-glue-fat-alias` (double capture -> TUR-E0005). Suite 2254/0;
> flag-off byte-identical.
>
> **What this still does NOT do (remaining for an httpd marker to drop):**
> 1. **Rewire httpd/reactor/CPS teardown.** httpd's `free(handler)` (and the
>    reactor `owns_cb` free, and `__dk_reap_ptr`) still bare-free a now-headered
>    handle -- so httpd cannot be built flag-on yet without corruption. Rewiring
>    them to `TUR_CLOSURE_DROP` requires the macro to be emitted unconditionally
>    (flag-off it expands to the identical `free`), which churns every codegen
>    snapshot -- a mechanical same-PR regen, deferred out of THIS slice to keep it
>    churn-free.
> 2. **The CORS strings.** mw-cors captures strdup'd strings as `cstr` (not
>    walked); they need owned `String` captures. Orthogonal to the closure walk.
> 3. **Cross-function double-ownership caveat.** The move closes same-function
>    aliasing; a caller that independently frees a handle it also passed in would
>    still double-free. Not a current pattern (flag-off such handles just leak), but
>    a general fix wants the move to thread across the call boundary.
>
> **Progress note (2026-07-21f) -- Model R: uniform header across every fat
> representation (`__tur_fatshim` / poly-to-fat boxes), + the type-honesty wall
> pinned for the nested-closure walk.** Two findings:
>
> - **Landed: multi-representation headers.** The prepend-header now also wraps the
>   `__tur_fatshim` box (bare-fn-to-fat, `{shim, orig_fn}`) and the poly-to-fat box
>   (`{shim, fn, env}`), flag-on. Both own nothing walkable, so their header is
>   NULL and `tur_closure_drop` frees the base. This makes `TUR_CLOSURE_DROP`
>   release ANY fat handle uniformly regardless of representation -- the
>   prerequisite for a walk that recurses a captured handle without statically
>   knowing which representation it is (the documented "give the `__tur_fatshim`
>   box its own header so a bare-fn handler is safe" item). Fixture
>   `closure-drop-glue-fatshim` (a bare fn shimmed to `^fat` then TUR_CLOSURE_DROP'd)
>   is leak-clean, no corruption. Suite 2252/0; flag-off byte-identical.
>
> - **Pinned: the nested-closure walk is blocked on TYPE-HONESTY, not just move
>   analysis.** Auto-deriving the walk needs to identify which captures are owned
>   closure handles. But the middleware `_n` capture comes from a `^fat next : int`
>   parameter -- erased to `TY_INT`, indistinguishable from a scalar. No type
>   predicate can pick it out, and `TY_PTR_VOID` is ambiguous (any pointer). So the
>   walk cannot fire on the httpd shape until `^fat` is carried as an owning-closure
>   TYPE (un-erased) rather than an `int` carrier -- a type-system change. A
>   type-HONEST `boxed TY_FN` capture COULD be walked (move-gated), but such
>   captures are rare in the corpus.
>
> - **Flag-on free-site boundary (audited).** Header-on programs are sound only
>   where every fat-handle free routes through `TUR_CLOSURE_DROP`. Still doing a
>   bare free of a (now-headered) handle, and therefore needing header-aware
>   rewiring before flag-on is safe for those programs: the CPS boundary reap
>   (`__dk_reap_ptr`, `emit_cps_ir.c`), the reactor `owns_cb` free
>   (`src/async/reactor.c`), and httpd's `free(handler)` (`stdlib/httpd.tur:949`).
>   The flag stays opt-in for simple programs (no CPS-reap / reactor / httpd
>   teardown of headered handles) until these are rewired.
>
> **Progress note (2026-07-21e) -- Model R walk slice: rc-capture retain/release
> (the unconditionally-sound half of the owning-capture walk).** The env drop-glue
> now WALKS refcounted owning captures, so an rc-capturing closure participates in
> the rc lifecycle regardless of escape:
>
> - **Retain at capture** (`emit_expr.c` env-fill): flag-on, storing an rc-typed
>   capture emits `rc_strong_increment(env->field)` -- the closure holds its own
>   strong count.
> - **Release in drop-glue** (`emit_fns.c` + the `emit_expr.c` fallback, kept in
>   lockstep): `drop_glue_<env>` emits `rc_strong_decrement` + `rc_free_queue_drain`
>   per rc capture (reverse order) before freeing the base.
> - **Soundness:** this is finding-#1's "retain when duplicated" for the capture
>   kind where it needs NO move/uniqueness analysis -- rc counting is aliasing-safe,
>   so retain+release always balances. An ESCAPING rc-capturing closure that
>   flag-off dangles (the constructor's auto-drop frees the rc out from under it)
>   is now correct flag-on: the retain keeps the rc alive for the closure's life
>   and the drop-glue releases it exactly once.
> - **Verified:** fixture `closure-drop-glue-rc-capture` (`make-counter` returns an
>   rc-capturing closure; `strong-count` reads 1 while held; released on
>   TUR_CLOSURE_DROP) is leak-clean, no UAF, no double-free flag-on. Generated C
>   shows the paired increment/decrement. Full suite 2251/0 (flag off -> a snapshot
>   re-emits byte-identically; the two new fixtures run flag-on).
>
> **Still to do in the walk:** the NON-refcounted owning captures -- a raw
> nested-closure handle (the middleware `_n`), a `ref` -- still are NOT walked,
> because recursing one blind is the finding-#1 double-free without move/uniqueness
> analysis. That analysis (or refcounting the env, Model R proper) plus the httpd
> type-honesty layer (owned `String` CORS captures) is what remains before an
> httpd/reactor marker can drop.
>
> **Progress note (2026-07-21d) -- Model R ABI FOUNDATION landed behind
> `--enable=closure-drop-glue` (experiment, off by default).** The runtime
> drop-glue header + generic-release plumbing is in; the move-aware owning-capture
> WALK and the httpd type-honesty layer are the remaining increments. What landed:
>
> - **Experiment** `closure-drop-glue` (`src/runtime/experiments.c`,
>   `g_opt_closure_drop_glue`, prototype, introduced 0.30.1 / expires 0.34.0).
>   Off by default -> the base language is byte-for-byte unchanged (verified: a
>   sample snapshot re-emits identically; full suite 2250/0 with the flag off).
> - **Prepend-header ABI (contained variant).** Flag-on, every heap
>   `struct __env_N` is allocated as `malloc(sizeof(void*) + sizeof(env))` with the
>   fat pointer handed back PAST an 8-byte header; `env[-1]` holds the env's
>   `drop_glue_<env>` pointer. `fat[0]` dispatch, capture-by-field access, and the
>   escaping handle are all byte-identical to the headerless layout -- chosen over
>   inserting a slot at `[1]` precisely because `[1]` is the fn slot in the
>   `__tur_fatshim` / `tur_poly_fn_t` representations, so an inserted slot would
>   collide (emit_module.c:678, :5910). Box internals stay untouched; only `[-1]`
>   is new. (`src/compiler/emit_expr.c`, `src/compiler/emit_fns.c`.)
> - **Generic release.** `TUR_CLOSURE_DROP(h)` (preamble, emitted only under the
>   flag) recovers `h[-1]` and calls it; `drop_glue_<env>` frees the base
>   allocation. The scope-exit env free (`let_binding_env_freeable`) routes through
>   it flag-on. Fixture `closure-drop-glue-model-r` (a `flags` file enables the
>   experiment) proves construction+dispatch+generic-drop is leak-clean and
>   corruption-free.
>
> **Remaining increments (both needed before any httpd/reactor marker can drop):**
>
> 1. **Move-aware owning-capture WALK.** `drop_glue_<env>` currently frees only the
>    base -- it does NOT recurse owning captures, because doing so blind is the
>    finding-#1 double-free (a captured closure/rc the caller still owns would be
>    freed twice). The walk must gate on move/uniqueness (`is_moved` /
>    `is_unique_consumed` / a fresh capture-clone) so it recurses ONLY when the env
>    is the capture's sole owner. This is what actually reclaims a middleware
>    chain's inner `_n` env.
> 2. **httpd type-honesty + teardown wiring.** The httpd handler reaches an opaque-C
>    holder as a type-erased `:int` and its CORS strings are secretly-owned `cstr`;
>    neither is walkable until `_n` is a recognizable owned-closure capture and the
>    strings are owned `String`. Then rewire httpd's `free(handler)` (~line 949)
>    and the reactor callback frees to `TUR_CLOSURE_DROP`, and give the
>    `__tur_fatshim` (rep-2) box its own header so a bare-fn handler is safe. Until
>    every env free site on those paths is header-aware, the flag stays opt-in for
>    simple programs (no reactor/httpd/CPS-env-reap).
>
> **Progress note (2026-07-21c) -- S1 + S2/Model U closeout; Model R scoped &
> deferred.** After the 2026-07-21b marker sweep, a full audit of every remaining
> `requires.no-leak-check` fixture (rebuilt suite-faithfully, LSan on) established
> that NO tractable closure-env leak remains -- the only closure-family opt-outs
> left are the httpd/reactor server closures, and their sound elimination is
> blocked on the Model R ABI, not on any missing S1/S2 analysis. Concretely:
>
> - **Type erasure is the wall.** `compose-middleware-of` returns `:int`
>   (`stdlib/httpd.tur`), so at `(httpd-new-async 0 composed)` the compiler sees a
>   bare `:int` handler with no env type -- it cannot emit or select the right
>   `drop_glue_env_N`. Model U (the holder's *generated* drop glue frees the
>   field) requires the holder to be a Turmeric struct/ADT whose field type names
>   the closure; httpd's holder is an opaque C `struct { int64_t handler; ... }`.
> - **Sound-without-clone, for these captures.** The httpd chain's captures are
>   either scalar (`donech`) or uniquely-owned-fresh (the `httpd-cors-own-str`
>   strdup'd strings; the `_n` next-closure handle, moved in) -- none alias an
>   outer owner that also drops them, so a walk-glue free of them would NOT
>   double-free. The blocker is purely *dispatch* (how opaque C names the glue),
>   not the finding-#1 capture-clone hazard. That hazard remains real for the
>   general case (capturing a live `rc`), so a general owning-capture walk-glue
>   still needs capture-clone first -- but the httpd family does not.
> - **Decision:** do not force the Model R env-layout ABI change speculatively
>   (it perturbs every closure's env struct + `TUR_APPLY` dispatch assumptions +
>   HKT poly-thunk recovery + `tur_poly_fn_t` + WASM glue, and churns every
>   closure snapshot) to reclaim a documented process-lifetime allocation. Keep
>   the httpd/reactor markers; land Model R only when that family is explicitly
>   prioritized, using the sketch below.
>
> **Model R -- ready-to-execute sketch (contained variant).** Because httpd
> handlers are monomorphic one-word envs (not poly/HKT), the change can be scoped
> to the plain `struct __env_N` path and leave `tur_poly_fn_t` untouched:
>
> 1. **Env header:** emit `struct __env_N { int64_t __fn; void (*__drop)(void *);
>    <captures> }`. `__fn` stays at offset 0 so every `fat[0]` dispatch
>    (`TUR_APPLY*`, httpd/reactor C) is unchanged; captures shift by one word but
>    are always field-accessed, so lifted thunks are unaffected. Audit for any raw
>    `fat[1]`/offset-1 capture read (there should be none).
> 2. **Glue:** emit `drop_glue_env_N(void *p)` per env type -- free owning `cstr`
>    captures, recurse `((struct{int64_t __fn; void(*d)(void*);}*)cap)->d(cap)`
>    into nested fat-closure captures, then `free(p)`. Fill `tmp->__drop =
>    drop_glue_env_N` at construction (or `NULL` for a scalar-only env -> bare
>    free).
> 3. **Teardown:** replace `httpd.tur`'s bare `free((void *)handler)` (~line 949)
>    and the reactor callback frees with
>    `{ void(*d)(void*) = ((...__drop layout...)handler)->__drop; if (d) d((void*)handler); else free((void*)handler); }`.
> 4. **Capture-clone (only if generalized):** for the general owning-capture case
>    (an env capturing a live `rc`/`ref`/nested closure the caller still owns),
>    add capture-time retain/clone at the env fill BEFORE enabling walk-glue on
>    that capture kind, per finding #1. NOT needed for the httpd family.
> 5. **Regen** every closure snapshot in the same PR (env struct + `__drop` fill).


> **Progress note (2026-07-21b) -- S2 exit-gate marker cleanup: six
> `requires.no-leak-check` markers DROPPED (now verified leak-clean under LSan;
> suite 2249/0).** The landed S1/S2 drop machinery has made the escaping /
> HOF-passed value-closure fixtures ASan-clean, so their leak-check opt-outs are
> stale and removed. Each was rebuilt exactly as `tests/run.sh` does
> (`-O2 -L<tur>/src`, ASan/LSan-instrumented output binary) and confirmed to emit
> ZERO LeakSanitizer output:
>
> - **S1/S2 exit-gate fixtures** (`free-lift-bind`, `unsafe-closure-capture`,
>   `cps-backend-fn-param`): the plan's "become ASan-clean and DROP their
>   `requires.no-leak-check` markers" gate item -- now satisfied. (The residual
>   free-monad `Suspend` ADT 16 B leak that kept `free-lift-bind` marked is also
>   gone.)
> - **S2 stored-closure fixture** `hkt-stdlib-parser-instances`: the closure
>   stored in a `Parser` value is now reclaimed -- the "flip ... and become
>   leak-clean" gate item for it is met.
> - **Fat-closure-dispatch regressions** `ascribe-fat-closure-call` /
>   `fat-closure-ascription`: their `make-adder` escaping-env leak, which the
>   marker only ever papered over, is now reclaimed.
>
> Also re-verified: the `make-scaler` minimal repro from
> `escaping-fat-closure-env-leak.md` is ASan-clean, and `currying-effect-partial`
> is green. Full `bash tests/run.sh` = 2249 passed, 0 failed with the six markers
> removed (i.e. those fixtures now run WITH leak detection on).
>
> **Still open (markers RETAINED, correctly):** the **httpd middleware family**
> still leaks -- `httpd-async-mw-compose` (64 B / 5 allocs) and the `httpd-mw-*`
> set (16-64 B each). Root cause confirmed: `httpd.tur` (~line 949) tears down the
> stored handler with a bare `free((void *)handler)`, which reclaims only the
> OUTER env word of a `compose-middleware` chain -- it neither recurses into the
> chained `_n` next-closure envs nor drops the env's OWNING captures (the strdup'd
> `const char *` CORS/header strings). Freeing those needs the `drop_glue_env_N`
> walk-glue AND a way to dispatch it from opaque hand-written C (a drop-glue
> pointer in the fat handle, or Model U with the async server as a
> generated-drop holder) -- both still unbuilt (finding #1: captures are stored
> without a retain/clone, so a naive walk-glue would double-free). So those
> markers stay until the walk-glue + capture-clone unit lands.
>
> **Progress note (2026-07-21) -- local fn-field struct drop LANDED (direct
> path); the "Remaining S2 gap" below is closed for uncolored functions.** A
> by-value struct local that owns a BOXED fn-field now frees that heap fat handle
> at scope exit. Mechanism:
>
> - `elab_forms.c` flags the local (`Binding.drops_fn_fields`) when it passes the
>   SAME moved / consumed / escape guards that admit the existing rc/ref
>   `byvalue-struct-field-leak` auto-drop (`elab_field_is_boxed_fnfield` +
>   `is_binding_consumed` / `is_field_consumed` / `binding_moved_during_init`), so
>   a struct that escapes (returned / moved / consumed) is never flagged.
> - The DIRECT emitter (`emit_let_value`) frees the box via a new
>   `drop_fnfields_<T>(&local)` glue (`emit_module.c`) -- fn-fields ONLY (rc/ref
>   are still discharged by the injected `(defer (drop! (.f o)))`), and NO
>   `free(&local)` (the struct is stack-resident).
>
> Crucially this is **not** a `(defer (drop! (.fn o)))`: an fn-field-drop defer
> reads a fat-fn field that the CPS/DK backend's continuation-capture admission
> rejects, evicting a COLORED fn to the retired direct/fiber path (hard build
> failure -- reproduced on `cps-backend-closure-local` with the defer approach).
> Emitting the free directly in the direct emitter leaves colored functions
> untouched: CPS lowering never runs `emit_let_value`, so a fn-field box in a
> colored fn leaks exactly as it did before local drops existed (no regression,
> no eviction). Uncolored functions release it.
>
> Verified valgrind-clean (definitely-lost 0, exactly-once free, no double-free)
> for: pure-fn-field local (call + drop), mixed rc+fn-field local (rc via defer +
> fn via emit), capturing-closure env box, and the escape case (a returned struct
> is NOT dropped by the producing fn). Suite 2220/0 (one snapshot regenerated:
> `defstruct-field-arrow`, whose local `Cell` fn-field box now frees). Fixture
> `local-struct-fnfield-drop`.
>
> **Still open:** (1) a fn-field box in a COLORED function still leaks (needs the
> CPS backend to admit an fn-field auto-drop, or a scalar-box-pointer capture
> form). (2) A boxed fn-field holding a capturing env with OWNING captures leaks
> those captures (only the env box is freed) -- the S1 walk-glue work. (3)
> Pre-existing, orthogonal: reading an rc field into a var (`(let [s (.r o)] ...)`)
> double-frees the control block -- the field read aliases without an incref while
> both `o`'s field-drop and `s`'s rc-drop decrement it. Filed separately.
>
> **Progress note (2026-07-20f) -- S2 Model U drop glue + move landed for
> fn-fields (rc-wrapped path verified sound).** A boxed fn-field is now an owning
> field: `resolve_ctor_field` sets `needs_drop_glue`, so the holding struct's
> by-value drop glue `free`s the field's heap fat handle (a capturing env, or the
> `{shim, fn}` box for a bare fn). Storing a CAPTURING closure variable into such
> a field MOVES it (`binding_mark_moved` in the constructor arg loop), so aliasing
> -- the same closure in two structs, which valgrind confirmed double-frees the
> shared env -- is now a compile-time `use-after-move` error. A thin fn re-shims
> to a FRESH box per store and an inline closure has no source, so neither is
> consumed. Verified valgrind-clean (0 errors, exactly-once free) for rc-wrapped
> closure structs, thin-fn structs, and rc-cloned structs; the struct-copy path is
> compile-rejected by rc uniqueness. Suite 2219/0 (one snapshot regenerated:
> defstruct-field-arrow). Fixtures `capturing-closure-struct-field` (store+call)
> and `errors/closure-struct-field-move` (aliasing rejected).
>
> **Remaining S2 gap (NOT closure-specific):** a LOCAL by-value fn-field struct
> does not invoke its drop glue at scope exit -- the same local-owning-value drop
> machinery that is deferred for `:heap` structs generally (see
> `docs/archive/drop-glue-shallow-nested-owning-aggregate.md`, verified only via
> rc-wrapping for the same reason). So a closure stored in a plain LOCAL struct
> still leaks its handle (no double-free -- just the pre-existing local-drop gap).
> When local-struct drop invocation lands, this S2 drop glue frees those too with
> no further work.
>
> **Update (2026-07-20e) -- the S2 blocker is FIXED; S2 is now unblocked.** Parts
> 1+2 of `docs/archive/capturing-closure-in-struct-field-segv.md` landed: a
> concrete `(fn ...)` struct/ADT field now uses the fat representation uniformly
> (field type `boxed`; make-struct shims thin fns to fat; field-calls dispatch via
> `TUR_APPLY*`). A capturing closure stored in a struct field now RUNS (no SEGV).
> Suite 2218/0; fixture `capturing-closure-struct-field`. fn-field values are
> intentionally uniformly HEAP-allocated (malloc'd fat handles) so the S2 drop
> glue below can free them uniformly -- so a stored fn/closure currently leaks its
> heap handle (shim box or capturing env) until that drop glue lands. That is the
> remaining S2 work, now buildable on a working store-and-call path:
>   - **S2 Model U:** storing a closure/fn into a struct field MOVES it (source
>     consumed; a second store is a move-check error, preventing the aliasing
>     double-free); the holding struct's drop glue frees the heap fat handle
>     (`free(field)` for the shim box or `drop_glue_env_N` for a capturing env).
>   Without the move check, a closure stored into two structs would double-free,
>   so drop glue must land WITH move semantics, not before.
>
> **Blocker note (2026-07-20d) -- S2 is blocked on a struct fn-field dispatch
> bug, NOT a leak.** Scoping S2 (Model U: a stored closure freed by the holding
> struct's drop glue) surfaced that the store-and-call path does not even work:
> storing a CAPTURING closure in a `defstruct` fn-field and calling it via
> `(.f box)` **SEGVs** -- the fat env pointer lands in the field but the read+call
> emits a THIN function-pointer call (`((R(*)(A))env)(args)`), executing the env
> as code (`emit_expr.c:1153/1470` fat-vs-thin keys on `type.as.fn.boxed` /
> `is_fat`, both false for a field read). A thin top-level fn in the same field
> works; only fat closures crash. Filed as
> `docs/reported/capturing-closure-in-struct-field-segv.md`. S2 CANNOT proceed
> until the field uses the fat representation uniformly (mark the field `boxed`;
> auto-shim thin fns to fat on store -- the "closure-representation-unification
> Phase 0" this plan already names). Freeing a stored closure is moot while it
> mis-dispatches. So the next S2 step is that unification bug, then move + drop
> glue on top.
>
> **Progress note (2026-07-20c) -- S1c fresh-closure-returning CALL args
> (headline `make-scaler` CLOSED).** The other half of S1c landed: a call to a
> fresh-closure-returning fn, passed to a non-retaining fn-param, is now hoisted +
> freed. New `Binding.returns_fresh_closure`, inferred when a fn is elaborated:
> its body is a bare capturing `EX_CLOSURE` with ONLY scalar (Copy) captures and a
> scalar result -- so every call mallocs a fresh, uniquely-owned env whose bare
> `free` is fully safe (no owning capture to double-free, result cannot alias the
> env). `hoist_borrowed_closure_args` (via a shared `arg_is_freeable_closure_source`
> predicate) and `let_binding_env_freeable` both accept such a call arg/init. The
> report's minimal repro `(use-it (make-scaler 2.0))` is now ASan/LSan-clean.
> Suite 2217/0 (3 snapshots regenerated -- kebab-case-capture + two bare-fat --
> where the same hoist now frees a previously-leaked env, all ASan-verified).
> Fixture `closure-env-free-fresh-returning-call`. Guards: a struct-storing
> callee and a fn returning an rc-capturing closure are BOTH correctly left
> unfreed (leak-safe, no UAF). **Still open:** S2 stored/escaping closures
> (httpd middleware, parser combinators; `cps-backend-fn-param`, `free-lift-bind`,
> `unsafe-closure-capture` keep `requires.no-leak-check` -- different shapes, not
> the fresh-consumed-once pattern).
>
> **Progress note (2026-07-20b) -- S1c inferred non-retention (INLINE args).**
> The non-retaining-callee half of S1c landed for INLINE capturing-closure
> arguments. A new `Binding.nonretain_param_mask` records, per fn-typed / `^fat`
> parameter, whether the callee body only CALLS it (inferred at defn elaboration
> via `!closure_binding_escapes(body, param)`). A body containing ANY inline-C is
> excluded (`expr_subtree_has_inline_c`, conservative default-true) -- C text can
> store a param invisibly to the AST, the exact unsoundness that first regressed
> `schema-transform-closure` + the httpd middleware set (they store `^fat` params
> via inline-C). The emit-side escape analysis and `hoist_borrowed_closure_args`
> both consult the mask, so an inline capturing closure passed to a non-retaining
> `^fat`/fn param is now hoisted + freed at scope exit (like the landed `^borrow`
> S1.2 path), no annotation needed. Suite 2216/0; fixture
> `closure-env-free-nonretain-fatparam`; guards verified ASan-clean (freed) and
> leak-safe (struct/inline-C/return retention conservatively NOT freed, no UAF).
> **Still open:** the report's headline `make-scaler` repro passes the closure as
> a CALL result `(use-it (make-scaler ...))`, not an inline `EX_CLOSURE`; hoisting
> a fresh-closure-returning CALL arg (make-scaler's binding already carries
> `returns_closure_fn_binding`) + letting `let_binding_env_freeable` accept a
> `ptr<void>`-typed fresh-env call init is the next slice. S2 (stored/escaping
> closures) unchanged.
>
> **Progress note (2026-07-20).** A second, adjacent leak landed:
> `binding_escapes_impl` (`emit_core.c`) fell to its conservative
> `default: escape` for `EX_DEFER` and the rc/weak/ref-family nodes (`EX_RC_OF`,
> `EX_WEAK`, ...). An owning let-binding lowers its auto-drop to a
> `(defer (drop r))` and its init is `(rc/of ...)`, so BOTH tripped the default
> and flagged every sibling closure as escaping -- a non-escaping closure's env
> leaked (16 B) in any `let` that also bound an `rc`/`ref`, even for a
> scalar-capture closure. Fixed by modeling `EX_DEFER` via its capture set and
> walking the rc/weak/ref operands (strictly more precise; never greenlights a
> free of a referenced env). Suite 2215/0; fixture
> `closure-env-free-with-owning-sibling`; write-up in
> `docs/archive/history/fat-closure-env-free-owning-sibling.md`. This is NOT one
> of the S1/S2 slices below (those are the ESCAPING / inline-HOF-arg cases); it
> is an orthogonal false-escape bug in the same env-free machinery.
>
> **Progress note (2026-07-19).** Verified against the tree: S1.2 is the only
> landed slice. `hoist_borrowed_closure_args` (`elab_call.c:773`), the
> `binding_escapes_impl` FA_BORROW relaxation (`emit_core.c:573`), and the
> `let_binding_env_freeable` scope-exit free (`emit_expr.c:1267`) are all
> present. The **`drop_glue_env_N` walk-glue does NOT exist** (no such symbol
> anywhere in `src/`), and there is no capture-time retain/clone -- so S1 (a/b/c)
> and both S2 models are entirely unbuilt. The `requires.no-leak-check` markers
> still sit on `free-lift-bind`, `unsafe-closure-capture`, `cps-backend-fn-param`,
> and `hkt-stdlib-parser-instances`; `currying-effect-partial` (re-classified out
> of S1) carries no marker. This plan remains **OPEN** -- the ownership feature
> (capture-clone + walk-glue) has not started.

## Landed: S1.2 -- borrowed HOF-arg closure free

A capturing closure passed INLINE to a `^borrow` fn-param now has its heap env
reclaimed at scope exit. `free-lift-bind` / `unsafe-closure-capture` (the
`(free-run (fn [inner] (* inner scale)) ...)` shape) dropped from a 32 B leak to
16 B -- the closure env is freed; the residual 16 B is a SEPARATE free-monad
`Suspend` ADT leak (not a closure), so those fixtures keep `requires.no-leak-check`
for that reason now. Suite 2179/0. Mechanism (no ownership hazard -- these
captures are scalar):
1. `free-run`'s interp param is annotated `^borrow` (it invokes but does not
   retain the closure -- a natural transformation is reused, so the CALLER owns
   and frees it, not the callee).
2. `binding_escapes_impl` (emit_core.c) treats a closure passed to a `FA_BORROW`
   param as NON-escaping (same only-greenlights-a-free posture as the box-accessor
   whitelist).
3. `hoist_borrowed_closure_args` (elab_call.c, applied in the `elab_call_fn`
   wrapper) hoists an inline capturing-closure `^borrow` arg into a fresh
   let-binding, so the existing `let_binding_env_freeable` scope-exit `free`
   reclaims it -- the inline env otherwise has no name to target.

Also fixed a pre-existing latent bug this surfaced: `elab_unsafe` allocated its
`HandleExpr` via `arena_alloc` and never initialized `shallow`, so effect_check
read an uninitialized bool (UBSan `load of value 190`); the arena layout shift
from the hoist made the garbage non-zero. Now `handle->shallow = false`.

Remaining S1: the OWNING-capture case still needs capture-time clone + the
`drop_glue_env_N` walk-glue (Implementation findings below); the `^borrow` free
here is hazard-free only because these payload captures are scalar.

**One-line:** give a captured ("fat") closure's heap env struct a real lifecycle
-- freed when the closure dies, dropped-through when stored, walk-glued when its
captures are themselves owning -- so escaping and HOF-passed closures stop
leaking and the two remaining B2 fixtures (`currying-effect-partial`,
`hkt-stdlib-parser-instances`) CPS-emit instead of evicting on `EX_CLOSURE`.

## What this unblocks

- **The escaping-fat-closure-env leak** (`docs/reported/escaping-fat-closure-env-leak.md`):
  one `malloc`'d `struct __env_N` leaked per capturing-closure construction that
  escapes (returned / stored / passed `^fat`). Currently carries
  `requires.no-leak-check` on `cps-backend-fn-param`, `free-lift-bind`,
  `unsafe-closure-capture`.
- **B2 residuals (2)** in the CPS backend: `currying-effect-partial` (a
  partial-application closure `add10 = (log-add 10)` called in a `Log` handle
  body) and `hkt-stdlib-parser-instances` (closures stored in `Parser` values).
  Both evict on `EX_CLOSURE` today because the closure cannot be admitted without
  a free.
- **The httpd middleware family** (`httpd-async-mw-attr` / `-mw-compose`) and any
  code that stores middleware/handler closures in a chain.

## Current state

A capturing closure lowers to (`emit_expr.c` ~5785):

```c
struct __env_N { int64_t __fn; <captures...> };
struct __env_N *tmp = malloc(sizeof(struct __env_N));
tmp->__fn = <thunk>; tmp->cap0 = ...; ...
```

The fat value carries `tmp` (a one-word env pointer, or a 2-word `tur_poly_fn_t`
for the rank-2 poly protocol). The ONLY free that exists today is
`let_binding_env_freeable` (`emit_expr.c:1267`): a let-bound closure is freed at
scope exit iff it is an **`EX_CLOSURE` literal**, returns a **scalar**, and
**provably does not escape** (`closure_binding_escapes`, conservative -- only ever
greenlights a free). Everything outside that narrow gate leaks:

- a **partial-application** closure (`(log-add 10)` -- init is a CALL, not an
  `EX_CLOSURE` literal),
- a closure passed as a **HOF argument** (`(free-run (fn ...) ...)` -- the arg is
  conservatively flagged escaping),
- a **stored / returned** closure (parser combinators, httpd middleware -- it
  genuinely escapes).

The CPS backend inherits this: it can only admit a capturing closure it can
free, so the un-freeable shapes evict on `EX_CLOSURE`.

## Two sub-problems (different fixes)

The residuals split cleanly by whether the closure ESCAPES its constructor:

**S1. NON-escaping closures that just aren't freed yet.** `currying-effect-partial`
(`add10` called once, locally), the `free-run` HOF args (`free-run` calls the
closure and discards it). These have a single owner and a clear scope-exit death
point; they leak only because the current gate is too narrow (`EX_CLOSURE`-literal
+ scalar-return + a conservative escape check that flags any call argument). Fix
is a scoped free, no ownership tracking.

**S2. ESCAPING closures.** `hkt-stdlib-parser-instances` (the closure is stored in
a `Parser` value that is returned / threaded), httpd middleware (stored in a
chain). Ownership transfers to the holder; the holder must drop it, and a closure
stored in two places must not double-free. Fix needs the closure to participate in
the drop / uniqueness system.

## Design

### The env drop-glue function (shared by S1 + S2)

Emit, per env type that needs it, a `drop_glue_env_N(void *p)`:

```c
static void drop_glue_env_N(void *p) {
    struct __env_N *e = (struct __env_N *)p;
    /* walk-glue: drop each OWNING capture in reverse order, mirroring the
     * ADT/struct drop-glue (emit_module.c emit_adt_byval_drop_glue). */
    <drop e->capK for each owning capture>   /* rc_strong_decrement / drop_glue_* / free */
    free(e);
}
```

- A **scalar-only** env (captures are all Copy scalars) needs no walk -- the glue
  is a bare `free(e)`; the current `let_binding_env_freeable` already emits that
  inline. The glue function matters when captures are themselves owning (an `rc`,
  a `ref`, a NESTED closure -- an env-in-env), exactly the case that leaks worst
  today.
- Reuse the existing owning-value drop machinery keyed off each capture's type
  (`needs_drop_glue`, `rc_strong_decrement`, `drop_glue_<adt>`), so a closure that
  captures an `rc<Foo>` decrements it, and a closure that captures another closure
  recurses into `drop_glue_env_M`.

### S1 -- scoped free for non-escaping closures

1. **Widen `let_binding_env_freeable`**: admit a partial-application closure (init
   is an `EX_CALL` whose `returns_closure_fn_binding` is set -- a curried under-
   saturation producing a closure) and drop the scalar-result restriction where
   the closure result cannot alias the env (needs the walk-glue so a non-scalar
   capture is dropped, not just the env freed).
2. **A HOF-arg free**: a closure passed as a call argument whose callee does NOT
   retain it (`free-run` calls-and-discards) can be freed after the call returns.
   This needs a callee "does not retain fn-param" property -- start conservative:
   a `^fat`/`(fn ...)` param that the callee only CALLS (never stores/returns) is
   non-retaining. `free-run`'s inline-C calls the interp once and returns an int
   -- non-retaining. Emit the env free after the call.
3. **CPS interaction**: the CPS backend already has the boundary-reap mechanism
   (`__dk_reap_ptr`, P3.c/P3.d). A non-escaping closure admitted on the CPS
   delegation path registers its env (and, via the glue, its owning captures) for
   reap at the entry boundary -- the analogue of `cps_closure_env_freeable`
   (which today handles only the scalar-capture let case). Wire the glue so the
   reap drops captures too.

S1 alone clears `currying-effect-partial`, `free-lift-bind`, `unsafe-closure-capture`
(dropping their `requires.no-leak-check`) without any ownership-tracking.

### S2 -- drop glue for escaping closures (the real feature)

An escaping closure is an owning heap value whose owner is the value it is stored
in (a struct field, an ADT payload, a return value). Two sound models:

- **Model U (uniqueness / move) -- preferred for STORED closures.** Plug the
  closure into the existing affine/move system (`is_moved`, `is_linear_consumed`,
  `is_affine`, `CK_MOVE`, the alias-state UT1 machinery). Storing a closure in a
  struct MOVES it (the source binding is consumed); the holding struct's drop
  glue (`needs_drop_glue`) calls `drop_glue_env_N` on the field. No refcount, no
  per-closure overhead; a double-store is a move-check error (as it already is for
  other affine values). This is how `hkt-stdlib-parser-instances` (closure stored
  in a `Parser`) and httpd middleware (closure stored in a chain node) should
  work -- the `Parser` / chain-node drop glue owns the closure.
- **Model R (refcount) -- fallback for genuinely SHARED closures.** Add a
  refcount word to the env (`struct __env_N { int64_t __rc; int64_t __fn; ... }`);
  a clone/dup increments, a drop decrements and runs `drop_glue_env_N` at zero.
  Uniform and sharing-safe, but adds a word + rc ops to every fat closure and an
  ABI change to the fat-closure protocol (the `^fat` layout, the HKT thunk
  recovery, `tur_poly_fn_t`). Reserve for closures the uniqueness model rejects
  (a closure legitimately shared by two owners).

Recommendation: land **Model U** for the stored-closure cases (covers the corpus
residuals) and only reach for **Model R** if a shared-closure fixture appears --
the ABI cost of R is high and the corpus does not yet need it.

## Phasing

Phasing describes ORDER of implementation, not permission to stop between
phases. Phase 1 is the immediate deliverable; Phase 2 follows in the same
push. There is no "land Phase 1 and hand back" -- the exit gate is the
suppressed fixtures going clean, which spans Phases 1 and 2.

- **Phase 1 (S1) -- one atomic ownership unit (see Implementation
  findings).** Order forced by the double-free hazard:
  (1a) capture-time retain/clone for OWNING captures (a bare capture aliases
  today, so an env-drop would double-free), (1b) `drop_glue_env_N` walk-glue on
  top, (1c) the non-retaining-callee (`^once`) annotation + a post-call free hook
  in EX_CALL emission for inline HOF args. 1a+1b are atomic (1a alone leaks MORE;
  1b alone double-frees). Clears the two PD leak fixtures (scalar-capture, so 1c +
  the emit hook, not the walk-glue, is what they need). NOT a "start here quick
  win" -- it is the ownership feature. `currying-effect-partial` is RE-CLASSIFIED
  out of S1 (it is a partial-app of a colored fn -- a B1-style colored closure,
  not a value closure).
- **Phase 2 (S2 / Model U):** closures participate in the move system; struct/ADT
  drop glue drops closure-typed fields via `drop_glue_env_N`. Clears
  `hkt-stdlib-parser-instances` and the httpd middleware family.
- **Phase 3 (S2 / Model R):** refcounted env for genuinely shared closures.
  This is a fallback ALTERNATIVE to Model U, not deferred work on the critical
  path -- Model U is the chosen path and covers the entire current corpus. Model
  R is only built if a fixture appears that Model U's move-check genuinely cannot
  express (a closure legitimately shared by two owners); its ABI cost is why it
  is the fallback, not the default. It is NOT a reason to leave Phases 1-2 open.

## Implementation findings (verified before starting S1)

A tractability pass on S1 established that it is NOT a quick bounded slice -- every
sub-path has either a soundness hazard or needs new analysis/machinery. Three
facts, each verified against the emitter:

1. **Capturing an owning value does NOT clone it.** The env-fill emission
   (`emit_expr.c` ~5793) is a bare `fat_tmp->field = <value>;` per capture -- no
   `rc` increment, no closure retain. So the walk-glue (dropping owning captures
   in `drop_glue_env_N`) is UNSOUND on its own: dropping a captured `rc` that the
   original owner still drops is a double-free. **The walk-glue REQUIRES
   capture-time retain/clone first** (the "retain when duplicated" half of the
   fix). Scalar (Copy) captures are safe (no ownership) -- so a scalar-only env
   drop is a bare `free`, hazard-free; an owning-capture env drop is blocked on
   capture-cloning.

2. **No post-call free hook exists for inline HOF-arg closures.** `free-lift-bind`
   / `unsafe-closure-capture` pass the closure INLINE to `free-run` (not a let
   binding), so `let_binding_env_freeable`'s scope-exit free (the only closure
   free that exists) does not reach it. Freeing it needs (a) a new "free this
   malloc'd env after the enclosing call/statement" mechanism in the EX_CALL
   emission, AND (b) proof the callee does NOT retain the closure -- `free-run` is
   inline-C whose non-retention is not analyzable; it needs a `^once`/non-retaining
   fn-param annotation or a whitelist. Even though these closures capture only a
   scalar (hazard-free to free), the emit hook + non-retention property are real
   prerequisites.

3. **`currying-effect-partial` is a partial-application of a COLORED fn.** `add10 =
   (log-add 10)` where `log-add` performs `Log`; the "closure" performs when
   called, so it is not a value closure at all -- it belongs with the B1-style
   colored-call handling, not S1 value-closure drop. It should be re-classified
   out of S1.

Net revised S1 order: (a) capture-time retain/clone for owning captures, then (b)
`drop_glue_env_N` walk-glue on top of it, then (c) the non-retaining-callee
annotation + post-call free for HOF args. Only step (a) unblocks a hazard-free
`drop_glue_env_N`; steps done out of order double-free.

## Risks / open questions

- **Double-free** is the cardinal risk. The escape analysis
  (`closure_binding_escapes`) is conservative (only greenlights a free), which is
  the right posture -- extend it carefully; a false "does not escape" frees a live
  env. Model U's move-check is the structural guard for S2.
- **Non-retaining callee property (S1.2):** deciding a callee does not retain its
  fn-param. Start with the syntactic "only calls it" rule (covers `free-run`);
  a general effect/escape signature on fn-params is a larger analysis -- keep it
  out of Phase 1.
- **Walk-glue ordering / cycles:** a closure that captures itself (letrec self-
  capture, already handled specially at construction -- `emit_expr.c` "Edge 1")
  must not recurse infinitely in the glue; mirror the ADT walk-glue's
  cycle-awareness or exclude self-captures from the drop walk.
- **Fat-closure ABI (Model R only):** adding an `__rc` word changes the `^fat`
  layout, HKT thunk recovery, and `tur_poly_fn_t`. Audited in
  `docs/archive/fat-closure-abi-audit-plan.md` -- coordinate there if R is ever
  needed.
- **`tur_poly_fn_t` (2-word) vs one-word env:** the drop must free the right
  object for both the plain env-pointer closures and the rank-2 poly-fat
  closures; confirm which allocation each frees.

## Test targets & exit gate

- `currying-effect-partial`, `hkt-stdlib-parser-instances` flip from
  `BODY-UNSUPPORTED` (`EX_CLOSURE`) to CPS-emitted (direct == cps == turi).
- `free-lift-bind`, `unsafe-closure-capture`, `cps-backend-fn-param` become
  ASan-clean and DROP their `requires.no-leak-check` markers.
- The minimal no-effects repro in `escaping-fat-closure-env-leak.md`
  (`make-scaler`) is ASan-clean.
- httpd middleware fixtures stay green and leak-clean.
- Full `bash tests/run.sh` green; the report moves to `docs/archive/` when closed.
