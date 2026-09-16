# stdlib: `:int` stand-ins for callbacks and container payloads

**Status:** Reported
**Severity:** Design defect / expressiveness hole. Highest subset: **38 callback
parameters whose signature is entirely unchecked** -- any arity, any argument
types, any return type is accepted. Second subset: **19 container/cell payload
parameters declared `:int`**, which cannot carry a `float` at all and accept a
by-value struct that then fails in cc.
**Discovered:** 2026-09-16, while auditing runtime seams for
[the seam family](router-payloads-are-int64-only.md).
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
   with a tag the way `any` does (`tests/fixtures/any-box-struct/`). Worth
   sequencing against
   [docs/upcoming/end-to-end-monomorphization-plan.md](../upcoming/end-to-end-monomorphization-plan.md)
   rather than patching each container.
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

## See also

- [docs/archive/spices-int-stand-in-audit-2026-06-14.md](../archive/spices-int-stand-in-audit-2026-06-14.md)
  -- the spice-side twin, same rubric, 35 spices.
- [router-payloads-are-int64-only](router-payloads-are-int64-only.md),
  [generator-yield-payload-is-int64-only](generator-yield-payload-is-int64-only.md),
  [async-await-payload-is-int64-only](async-await-payload-is-int64-only.md),
  [session-payloads-are-int64-only](../archive/session-payloads-are-int64-only.md) -- the
  same erasure where it is SILENT rather than declared.
- [CLAUDE.md](../../CLAUDE.md) -- "No Lazy `:int` Stand-Ins -- STRICT RULE".
