# stdlib: `:int` stand-ins for callbacks and container payloads

**Status:** Reported
**Severity:** Design defect / expressiveness hole. Highest subset: **38 callback
parameters whose signature is entirely unchecked** -- any arity, any argument
types, any return type is accepted. Second subset: **19 container/cell payload
parameters declared `:int`**, which cannot carry a `float` at all and accept a
by-value struct that then fails in cc.
**Discovered:** 2026-09-16, while auditing runtime seams for
[the seam family](../archive/router-payloads-are-int64-only.md).
**Scope:** `stdlib/*.tur` only.

This is the **stdlib twin** of
[docs/archive/spices-int-stand-in-audit-2026-06-14.md](../archive/spices-int-stand-in-audit-2026-06-14.md),
which audited 35 spices and explicitly scoped itself to
`../turmeric-spices/spices/*/src/**`. The stdlib was never swept. Its S1-S4
rubric is reused below so the two are comparable, with one class added (S2) that
the spices audit did not need.

Everything below is measured against `./build/tur` built from `main` 145df1d49,
2026-09-16, Apple clang 21.0.0 (arm64-apple-darwin27).

## Two measurements that change how this reads

Both correct a naive reading of "it's declared `:int`, so the checker protects
you." Neither is what I expected going in.

**1. `^fat f : int` enforces callable-ness but nothing about the shape.** A bare
integer is rejected:

```
error: argument 1 to fat (^fat) parameter of 'sink' must be a function or
       closure, got int
```

but every one of these passes `tur check` with exit 0 into `(defn sink [^fat
handler : int] : int handler)`:

```turmeric
(sink (fn [a : cstr b : cstr c : cstr] : cstr a))   ; 3 args, all wrong types
(sink (fn [] : float 7.25))                          ; nullary, float result
(sink (fn [a : int b : int c : int d : int] : int a)) ; 4 args
```

So the protection is "is it callable", and that is all. This is better than the
spices audit's S1 wording implies (a non-callable *is* caught) and still leaves
handler shape completely unchecked -- which is the defect
[CLAUDE.md](../../CLAUDE.md) names by name, since `tour-tourist` shipped
`(fn [req : int] : int)` where it meant
`(fn [ctx : Ctx] : option<Response>)`.

**2. A payload parameter declared `:int` is loud for `float` and UNSOUND for
aggregates.** The two failure modes are not the same severity:

```
$ ./build/tur check chan-float.tur
error [TUR-E0001]: function 'chan-send' arg 2: expected int, got float   <-- loud

$ ./build/tur check chan-struct.tur      # (chan-send ch (Pt 42))
                                          <-- exit 0, ACCEPTED
$ ./build/tur run chan-struct.tur
error: passing 'tur_adt_Pt' (aka 'struct tur_adt_Pt') to parameter of
       incompatible type 'int64_t'
  8309 |   chan_hysend((void *)(intptr_t)(ch_1635), __ps_276);
```

A `:int` payload therefore does not merely fail to express a float -- it lets a
by-value struct through the type checker and into a cc error naming
`tur_adt_Pt`, on a program the Turmeric checker accepted. That is the same
"checker accepted, cc refused" shape as the runtime-seam family, arrived at from
the opposite direction: there the erasure is in a runtime slot, here it is in the
declared signature.

## S1 -- Callback typed `:int` or `ptr<void>` (38 sites)

Highest priority. Signature unchecked per the measurement above.

| Spelling | Count | Files |
| --- | --- | --- |
| `^fat <name> : int` | 24 | `httpd.tur` (17), `free.tur` (3), `httpd-compress.tur` (2), `reactor.tur` (1), `parsec.tur` (1) |
| `<name> : ptr<void>` | 14 | `hamt.tur` (4), `threadpool.tur` (2), `future.tur` (2), `timer.tur`, `thread.tur`, `test.tur`, `sync.tur`, `scheduler.tur`, `fiber.tur` (1 each) |

The `httpd.tur` cluster is the most consequential because it is the public
surface every web spice composes against -- `httpd-new`, `httpd-new-pool`,
`httpd-new-tls`, `httpd-new-async`, `router-add` all take `^fat handler : int`.
The spices audit's central observation applies verbatim: **new code written
against these APIs inherits the looseness**, because a downstream wrapper
parrots whatever the upstream signature says.

Note `httpd.tur:3469` already documents `httpd-handler-carrier` as the
sanctioned way to hand a composed `Handler` to "a server constructor (`^fat
handler : int`) or any `:int` handler sink that TAKES OWNERSHIP" -- so there is a
real, ownership-aware `Handler` type in that module, and the `:int` sinks are
what it has to degrade into. That makes httpd the cheapest S1 fix in the file:
the type already exists.

The `ptr<void>` variants are worse than the `^fat` ones in one respect --
`ptr<void>` does not even get the callable-ness check, since it is not a fat
parameter.

## S2 -- Container/cell payload declared `:int` (19 sites, 6 modules)

New class; the spices audit did not need it. Not a handle and not a callback --
the **value being stored**. Consequence: the container cannot carry a `float`,
`cstr`, or struct at all.

| Module | Sites |
| --- | --- |
| `chan.tur` | `chan-send`, `chan-recv`, `async-chan-send`, `async-chan-recv`, `async-chan-try-send`, `async-chan-try-recv` |
| `atomic.tur` | `atomic-new`, `atomic-load`, `atomic-store!`, `atomic-swap!`, `atomic-cas!` |
| `future.tur` | `promise-fulfill`, `promise-fail`, `future-of`, `future-error-of` |
| `ref.tur` | `ref-new`, `ref-get` |
| `fiber.tur` | `fiber-yield` |
| `backtrack-dfs.tur` | `dfs-set` |

**Deliberately excluded** as genuine integers, per the rule's own last bullet:
`chan-new`/`async-chan-new` `cap`, `atomic-add!`/`atomic-sub!` `delta` (atomic
*arithmetic* -- an int is the point), `async-chan-count`, and
`dfs-choose-int`'s `lo`/`hi`.

This class is the honest-but-crippled sibling of the runtime-seam family: you
cannot put a float in a channel, which is loud, rather than putting one in and
getting `7` back, which is silent. It is a smaller emergency and a larger
expressiveness hole -- "send a float between two threads" is not an exotic ask.

## S3 -- An ADT erased to `:int` in its own API

The type exists; the API talks `int` about it.

- **`either.tur`** is the clearest case: `left?`, `right?`, `from-left`,
  `from-right`, `either-map`, `either-map-left` all declare their Either
  parameter as `e : int`, and `either-map` takes `^fat f : (fn [int] int)` -- so
  an `Either` over any payload but `int` cannot be mapped at its own type, even
  though `match e (Left l) ... (Right r) ...` inside the body proves the ADT is
  real.
- **`free.tur`**: `free-pure [x : int] : int`, `free-fmap [free : int ^fat f : int]`.
- **`json.tur`**: `json/bool [v : int] : int` (a bool, as an int, returning a
  JSON value, as an int), `json/int`, `json/object-put [obj : int key : cstr val : int] : int`.
- **`csv.tur`** / **`csv-string.tur`**: `csv/emit-row [v : int]`, `csv/row-free [v : int]`.
- **`parsec.tur`**: `mbind [ma : int ^borrow ^fat fn : int] : int`.

`json/bool [v : int]` is worth singling out because it is the rule's "is it a
boolean? -> `:bool`, never `:int` with 0/1 convention" bullet, in the stdlib,
on a public constructor.

## NOT defects -- do not "fix" these

A sweep like this flags them, and changing them would be wrong. Recorded so the
next person does not.

`(defopaque List [A] :int)`, `Backtrack [A]`, `Kleisli [A B]`, `Zipper [A]`,
`NonEmpty [A]`, `SizedBuf [n]`, and the `:ptr<void>` variants `Goal [A]` and
`Parser [A]` are **phantom-typed newtypes over a carrier**, which is the
representation the typed path deliberately chose. `list-typed.tur:20` says so
outright -- "a typed view over the int64 cons-list carrier, carrying a phantom
element type A ... Since: end-to-end-monomorphization-plan (Phase 1.1)". The
carrier *is* the pointer; the type parameter supplies the static distinction the
rule asks for. Measured: `(list-of 7.25 1.5)` checks and runs clean.

`List` exposing only `list-empty?`, `list->carrier` and `list-count` with no
typed element accessor is a **feature in progress**, not an erasure defect --
element reads still go through the raw carrier because Phase 1.1 is where it
stopped.

Likewise, the per-file `: int` counts are a scale signal and **not** a defect
count: `httpd.tur` 50, `schema.tur` 42, `range.tur` 38, `parsec.tur` 35 include
many genuine lengths, counts, ports, and indices.

## Fix directions

1. **S1 first, starting with `httpd.tur`.** Spell the handler type:
   `^fat handler : (fn [Request] Response)` (or whatever the module's real shape
   is). httpd already has an ownership-aware `Handler`, so this is mostly
   propagating a type that exists. 17 of the 24 `^fat : int` sites are in one
   file.
2. **`ptr<void>` callbacks get real `:fn` types** -- these do not even get the
   callable-ness check.
3. **S2 wants a decision, not a mechanical rewrite.** Making `chan-send`
   parametric in its payload is the right answer and is the same design question
   the runtime-seam family raises: either monomorphize per payload type, or box
   with a tag the way `any` does (`tests/fixtures/any-box-struct/`).

   > **Corrected 2026-09-18.** This item originally said to sequence S2 against
   > `docs/upcoming/end-to-end-monomorphization-plan.md`. **That plan is
   > finished, and was already finished when this report was filed** -- it and
   > its successor were archived 2026-06-19, the successor's banner reading "End
   > to-end monomorphization landed ... the small ABI bridge that remains is
   > intentional and necessary, with no further work to be done on it". The
   > `../upcoming/` path had been dead for three months. So S2 is **not blocked
   > on a pending ABI decision**; there is nothing to sequence against, and the
   > question below is the real one.
4. **Until S2 lands, reject aggregates at the boundary.** The struct-through-a-
   `:int`-parameter case reaching cc is a plain soundness hole and is
   independently fixable: a by-value aggregate passed to a declared `:int`
   parameter should be a Turmeric diagnostic, not a `tur_adt_Pt` message from
   the C compiler. This is the cheapest item in the report and it protects every
   S2 and S3 site at once.
5. **S3 is per-module cleanup** and can follow at leisure, except `json/bool`,
   which should just be `:bool`.

Direction 4 is the floor. Everything else is a preference per the rule's own
"when you notice this in existing code" clause -- on the one track to v1 these
are matched, wrapped, or tightened as the work dictates, not a gate.

## Direction 4 reassessed 2026-09-18 -- it is NOT the cheapest item

**Attempted and reverted.** The claim above that direction 4 is "the cheapest
item in the report", "independently fixable", and "protects every S2 and S3
site at once" does not survive contact. The hole is real and still reproduces
(`tur check` exit 0 on a by-value aggregate into a declared `:int` parameter,
verified against v0.49.4), but **no type-level rule in the elaborator can close
it without rejecting crossings that work today.**

What was tried, both as a `TUR-E0295` rejection at the `TY_ADT -> TY_INT` hatch
in `elab_call.c` (the `Phase G0` comment there -- "ADT values are heap-allocated
and passed as int64_t pointers" -- is the stale premise):

| Rule | Fixture result |
| --- | --- |
| baseline, unmodified | **3047 passed, 0 failed** |
| reject `type_is_byvalue_aggregate` (the predicate `::` already uses) | 2989 passed, **59 failed** |
| narrowed to single-variant flat products (`n_ctors == 1`) | 3027 passed, **21 failed** |

The 59 and the 21 are regressions, not pre-existing failures -- the baseline
was measured on the same tree with the change reverted.

**Why the elaborator cannot decide.** The failures are not broken programs. They
are legitimate crossings the *emitter* handles:

- `emit_expr.c:8370` heap-boxes a by-value aggregate argument (`emit_agg_box`)
  at poly-carrier / HKT-wrapper boundaries -- "Slice 3
  (constrained-hkt-forall codegen)".
- ADT **constructor** arguments take a carrier field and box into it:
  `stdlib/logic.tur:105` `(StCons v ...)` with `v : Subst` is a by-value
  aggregate into an `:int` ctor field, and it is correct.
- SR1 by-value **sums** qualify as "by-value products" to
  `adt_is_byvalue_product` (via `adt_sr1_sum_candidate`), so
  `tests/fixtures/typed-slots/adt-float-payload` -- whose own comment reads
  "through polymorphic boundary: pointer survives as int64, value unchanged"
  -- is rejected by the blanket rule while being exactly the behaviour the
  fixture asserts.

So "by-value aggregate meets `:int` parameter" is **not** the predicate for
"this will fail in cc". The emitter knows; elaboration does not. That is also
why [byvalue-adt-int-cast-plan](../archive/byvalue-adt-int-cast-plan.md) could
close GAP 3 cleanly for `::` -- there the reinterpret is unconditionally
unsound, with no boxing rule to consult -- and why its headline, calling `::`
"the one erased-carrier boundary that neither uses [the box bridge] nor rejects
the cast", reads as complete but is not: **the call-argument position is a
second such boundary, and it is the harder one.**

**Revised direction 4.** A general rule does not belong in `elab_call.c`; the
boxing decision is the emitter's. But there is a **working precedent that is
not general**, and it is the more useful lead:

`turi-session-expansion-plan` S3.5 closed the whole silent-erasure seam family
(router / generator / async-await / binary-session payloads, all four archived
2026-09-16) with a payload-lowering pair --
`session_payload_to_word` / `session_payload_from_word`
(`src/compiler/elab_sessions.c:303+`, called from `elab_global.c`,
`elab_forms.c`, `emit_module.c`, mirrored in `src/turi/eval.c`). A float is
bit-reinterpreted, a pointer cast through `intptr_t`, and **a by-value struct
is rejected with a diagnostic (TUR-E0212) rather than reaching cc** -- which is
precisely what direction 4 asks for, already shipped, at four seams.

Why it worked there and not as a general rule: a session `send-to` / `recv-from`
is a **compiler-lowered template**, so the compiler owns the lowering site and
can wrap the payload on the way through. The general call boundary has no such
site -- which is what the 59/21-regression measurement above is really saying.

**The open question for both S2 and direction 4** is therefore narrower and
answerable: `chan-send` / `atomic-store!` / `ref-new` are ordinary stdlib
`defn`s over inline-C, **not** compiler-lowered forms, so the pair does not
obviously transfer. Either give those seams a lowering site the compiler owns
(the session shape), or make the containers parametric. That is the decision to
take -- not a wait on a plan that finished in June.

**Repro kept here rather than as a fixture.** An `errors/` fixture for this
would be permanently red -- the tree has no xfail/expected-fail marker (the
`requires.*` family are skip conditions, not known-failure ones) -- and a
standing red fixture with no way to mark it invites someone to "fix" it by
shipping one of the two rules measured above. The repro is three lines:

```turmeric
(defdata Vec2 :copy (Vec2 :int :int))
(defn sink [v : int] : int v)
(defn main [] : int (println (sink (Vec2 3 4))))
```

`tur check` exits 0; `tur run` fails in cc with
`passing 'tur_adt_Vec2' to parameter of incompatible type 'int64_t'`.

## Done 2026-09-18

- **S3, `json/bool`** -- `(defn json/bool [v : int] : int)` is now
  `[v : bool]`, the one item the report called out as unambiguous ("should just
  be `:bool`"). The inline-C body's `v ? 1 : 0` is unchanged, no caller outside
  the generated docstring table existed, and `(json/get-bool (json/bool true))`
  prints `true` / `false` across the pair. Suite 3047/0.

## See also

- [docs/archive/spices-int-stand-in-audit-2026-06-14.md](../archive/spices-int-stand-in-audit-2026-06-14.md)
  -- the spice-side twin, same rubric, 35 spices.
- [router-payloads-are-int64-only](../archive/router-payloads-are-int64-only.md),
  [generator-yield-payload-is-int64-only](../archive/generator-yield-payload-is-int64-only.md),
  [async-await-payload-is-int64-only](../archive/async-await-payload-is-int64-only.md),
  [session-payloads-are-int64-only](../archive/session-payloads-are-int64-only.md) -- the
  same erasure where it is SILENT rather than declared.
- [CLAUDE.md](../../CLAUDE.md) -- "No Lazy `:int` Stand-Ins -- STRICT RULE".
