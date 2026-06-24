# valkey typed variadic per-command builder -- plan

**Status:** Done (P0 + P1 + P2 all landed). Optional Track C / U6
follow-up; U6 itself closed in c-dsl (turmeric-spices 45873c2 / `c269f23`).
The valkey side now also carries the typed variadic surface end to end:
`spices/valkey/src/valkey/cmd.tur` exports `ValkeyArg` with `VkStr` /
`VkInt` / `VkBytes` variants, smart constructors `vk-str` / `vk-int` /
`vk-bytes`, and the variadic `cmd [^borrow c : Client command : cstr
& args : ValkeyArg] : ptr<void>` forwarding to a fixed-arity
`__cmd-impl` that walks the cons list and dispatches on the runtime tag
into hiredis's `redisCommandArgv` (binary-safe via `argvlen[]`). The
README example block uses the new surface, and
`spices/valkey/tests/valkey/cmd_test.tur` exercises construction of all
three variants. P3 ("retire the cons-list `cmd`") is N/A -- there is no
cons-list `cmd` left to retire; the typed surface is the only surface.

Archived 2026-06-23.

**Tracks:** rjungemann/turmeric `docs/parallel-tracks.md` Track C / U6;
rjungemann/turmeric `docs/upcoming/spices-type-features-uplift-plan.md`
Phase U6. Lives in `v2/` because it is optional polish on a closed phase,
not a v1 ship requirement.

**Spice repo:** `rjungemann/turmeric-spices:spices/valkey/`.

---

## Background -- what `cmd` looks like today

`spices/valkey/src/valkey/cmd.tur` exports a generic dispatch:

```turmeric
(defn cmd [^borrow c : Client  command : cstr  args : int] : ptr<void>
  ```c
  /* walks the args cons list of int64-encoded cstrs and marshals into
     redisCommandArgv(ctx, argc, argv, NULL) */
  ```)
```

`args` is an **untyped cons list of `:int`** at the type system; at
runtime each cell's value is reinterpret-cast to `const char *`. Callers
write:

```turmeric
(cmd c "SET" (cons "mykey" (cons "myval" 0)))
```

Anything that is `:int`-shaped goes through. There is no type-level
distinction between a string argument and any other handle that happens
to lower to `:int`; a buggy caller can hand `cmd` a `Client` or a freed
reply pointer and the dispatch will happily pass it to hiredis, which
will then segfault or send garbage on the wire. The Redis protocol
*does* eventually reject malformed commands at the server with a
`WRONGTYPE` reply, but the boundary is the network -- the type system
gives no help getting there.

The per-command wrappers (`cmd-get` / `cmd-set` / `cmd-hset` / ...)
sidestep `cmd` entirely and call `redisCommand` directly with positional
typed parameters. They are fine; `cmd` itself is the type-eraser.

## Goal

Lift `cmd`'s argument list into a typed variadic so that

```turmeric
(cmd c "SET" (vk-str "mykey") (vk-str "myval"))
(cmd c "SET" (vk-str "mykey") (vk-bytes payload n))
(cmd c "EXPIRE" (vk-str "mykey") (vk-int 60))
```

each works, and a mis-kinded arg (e.g. passing a bare `Client` where a
`ValkeyArg` is expected) is rejected at the elaborator with the same
`variadic call to 'cmd': rest arg N has wrong type (expected ValkeyArg,
got T)` message c-dsl's U6 surface uses today.

## Non-goals

- Not rewriting `cmd-get` / `cmd-set` / `cmd-hset` / etc. on top of the
  variadic. Those already accept typed positional parameters and route
  through `redisCommand` (not `redisCommandArgv`); rewriting them buys
  no extra safety because the positional types already pin the shape.
  Leave them alone.
- Not adding new typeclasses to make `cmd c "SET" "key" "val"` work
  without a wrap. The whole rubric of U6 is "wrap so a wrong-kind is a
  type error"; auto-wrapping defeats that.
- Not designing a query DSL. `cmd` stays a one-shot send/recv;
  pipelining is its own separate U1 follow-up
  (`turmeric-spices:docs/parallel-tracks.md` "valkey pipelined-reply
  `^&out`").
- Not changing the wire-level marshalling. The body still calls
  `redisCommandArgv`; only the front-end type changes.

## Design

### Two-tier wrap: opaque newtype + small ADT

The c-dsl precedent (`CField`/`CParam`/`CEnumVariant`/`CTypeRef`) wraps
a single cstr per type. valkey needs to carry a heterogeneous mix
(string, integer, binary blob) and the wrap layer is the natural place
for the discriminator. Two viable shapes:

**Shape A -- defopaque + tag word.** One `defopaque ValkeyArg :int`
holding a pointer to a heap-allocated `struct { kind; data; len; }`.
Constructors `vk-str` / `vk-int` / `vk-bytes` allocate and tag.

- + ABI is identical to c-dsl's wrap pattern; no new turmeric surface needed.
- + One newtype to ascribe; no `defdata`.
- - Allocates per arg. For a 4-arg `HSET` that's 4 mallocs the old API
  did not do. Not catastrophic but measurable.

**Shape B -- `defdata ValkeyArg` sum.**
`(defdata ValkeyArg :copy (VkStr cstr) (VkInt int) (VkBytes cstr int))`.
Constructors are the variant constructors themselves; the variadic body
pattern-matches.

- + Zero allocation -- the constructors compile to immediate values
  (one carrier word + a tag).
- + Native Turmeric ergonomics; `match` reads naturally in the body.
- - Variadic body cannot contain inline-C (CLAUDE.md rule). The cons
  walk + `match` either goes through a fixed-arity helper that takes
  the rest cons (and reads each `ValkeyArg` value via the
  defdata accessors), or the variadic body itself does the walk in
  pure Turmeric and calls a fixed-arity inline-C helper to format /
  send.
- - `VkBytes` needs to carry both pointer and length. That's two ADT
  fields, both `:int`-shaped, no surprise -- but it does mean
  `vk-bytes` is `(VkBytes payload-ptr length)`, not a single value.

**Recommendation: Shape B (`defdata`).** The allocation argument is
load-bearing for any hot-path use of `cmd` (loops calling `cmd c "LPUSH"
...` per item); a heap allocation per arg makes the variadic strictly
worse than the old untyped cons list, which the user will notice. The
"no inline-C in variadic body" constraint is the same one c-dsl
already solved (`__c-defstruct-impl` etc.) -- it just means the actual
hiredis call moves into a fixed-arity helper that takes the rest cons,
and the variadic surface is a thin pass-through.

### Constructor surface

```turmeric
(defdata ValkeyArg :copy
  (VkStr   :cstr)        ;; UTF-8 / NUL-terminated string
  (VkInt   :int)         ;; will be itoa'd into a temporary buffer
  (VkBytes :cstr :int))  ;; (ptr, len) for binary-safe args

;; Smart constructors -- expected idiomatic surface, mirror c-dsl style
(defn vk-str   [s : cstr]              : ValkeyArg (VkStr s))
(defn vk-int   [n : int]               : ValkeyArg (VkInt n))
(defn vk-bytes [p : cstr  len : int]   : ValkeyArg (VkBytes p len))
```

### New `cmd` shape

```turmeric
(defn cmd [^borrow c : Client  command : cstr  & args : ValkeyArg]
        : ptr<void>
  (__cmd-impl c command args))

(defn __cmd-impl [^borrow c : Client  command : cstr  args : int]
                : ptr<void>
  ```c
  /* the existing walk, but each cons cell's value is a ValkeyArg
     pointer rather than a raw cstr. The tag discriminates:
       VkStr   -> strlen + payload
       VkBytes -> embedded length
       VkInt   -> snprintf into a per-call scratch buffer
     redisCommandArgv takes a parallel argvlen[] array so binary-safe
     bytes flow through without truncation at embedded NULs. */
  ```)
```

The shift to `redisCommandArgv`'s `argvlen[]` form (already in use --
see `cmd.tur:73`) means `VkBytes` is the only constructor that
"unlocks" new behavior; `VkStr` is the existing path with `strlen`.

### Migration story

The old `(cmd c "SET" (cons "k" (cons "v" 0)))` form **stops
compiling** -- the rest is typed `ValkeyArg`, so a bare cstr is
rejected. Callers update to `(cmd c "SET" (vk-str "k") (vk-str "v"))`.

This is intentional. The whole point of U6 is that the type error
becomes a compile error rather than a downstream surprise; a
backwards-compatibility shim that silently accepts cstrs would put us
back where we started.

Two consumers in tree are affected at search time of writing:

- `spices/valkey/tests/` -- if/when test fixtures are added. (No
  test directory exists yet; this plan adds one.)
- `spices/valkey/README.md` -- example block. Trivial doc update.

No spice outside `valkey/` imports `cmd`. (Confirmed by
`grep -rln "valkey/cmd" spices/ --include="*.tur"`.)

## Phasing

### P0 -- VkStr only, full surface

Land the smallest slice that closes the U6 rubric:

1. Define `defdata ValkeyArg :copy (VkStr :cstr)` plus
   `vk-str`. Export both from `valkey/cmd`.
2. Rename current `cmd` body to `__cmd-impl` (fixed-arity, inline-C,
   unchanged walk).
3. Define new variadic `cmd [^borrow c : Client  command : cstr
   & args : ValkeyArg] : ptr<void>` that forwards `args` to
   `__cmd-impl`.
4. Update `README.md` example.
5. Add `tests/cmd_typed_test.tur`:
   - positive: `(cmd c "PING" (vk-str "hello"))` returns ok with the
     `"hello"` reply.
   - negative-by-inspection: in this PR's description (not as a
     fixture -- there is no negative-fixture harness in the spice
     test runner yet, see Track C P5), confirm that
     `(cmd c "SET" "k" "v")` fails to typecheck with
     `expected ValkeyArg, got cstr`.

Closes the variadic-typed surface for the common case. Single-arg
`VkStr` covers ≥80% of in-the-wild use today; pure-string commands
(`GET`/`SET`/`DEL`/`HGET`/`HSET`/`LPUSH`/`LRANGE` etc. with stringified
ints) all keep working with one wrap per arg.

### P1 -- VkInt + scratch buffer plumbing

Add `(VkInt :int)` variant + `vk-int` constructor. Body change:

- When walking, a `VkInt` argument needs a `char *` for
  `redisCommandArgv`; allocate a small per-arg scratch buffer (24
  bytes is enough for `INT64_MIN`'s decimal form) inside the impl,
  build the argv pointer array pointing at scratch + payload mix,
  send, then free the scratch.

The scratch-buffer ownership is the only subtlety: scratch must
outlive the `redisCommandArgv` call but is freed immediately after.
Allocate a `char[argc][24]` block on the heap once per call, fill,
send, free.

### P2 -- VkBytes (binary-safe)

Add `(VkBytes :cstr :int)` + `vk-bytes`. Body change:

- The walk already builds `argvlen[]` because `redisCommandArgv`
  requires it. `VkBytes` simply records its own length in
  `argvlen[i]` instead of calling `strlen`. The wire is binary-safe
  for free.

P2 also unlocks Redis's actual binary-safe key/value semantics. This
matters for storing protobuf / msgpack / arbitrary blobs in Valkey
without base64.

### P3 (optional) -- retire the cons-list `cmd`

Once P0..P2 have been in place long enough that no remaining caller
uses the cons-list shape, delete `__cmd-impl`'s acceptance of bare-cstr
cells and require `ValkeyArg` everywhere. This is purely a cleanup;
the elaborator already enforces the type at the public surface.

## Rubric check (U6 plan §"when does a feature pay rent?")

> "Variadic `& xs : T`: A builder API today takes a `cons`-list of
> `:int` plus a runtime tag-check, where the elements are actually
> homogeneous."

The element kinds (string / int / bytes) are heterogeneous at the
Redis-protocol level but homogeneous through `ValkeyArg`'s sum -- which
is exactly the wrap the rubric calls for. The "runtime tag-check"
today is the fact that the hiredis side will report `WRONGTYPE` if a
non-string slips into a key slot; pushing that error to the elaborator
is the win.

Rubric is satisfied. The phase did not justify this as P0 in the
earlier plan because c-dsl alone satisfied the U6 phase deliverable
and shipping anything more was scope creep; this plan is the
follow-up if a downstream consumer ever asks for it.

## Open questions

1. **Should `cmd-set` / `cmd-hset` / etc. also accept `ValkeyArg`?**
   Today they take typed positional `cstr` params and pass through
   `redisCommand` (not `redisCommandArgv`). Re-routing them through
   the new `cmd` would let them carry `VkBytes` values, which the
   typed positional form cannot. Argument for: one binary-safe path,
   not two. Argument against: the typed positional form is *more*
   ergonomic for the common case (`(cmd-set c "k" "v")`) than
   `(cmd c "SET" (vk-str "k") (vk-str "v"))`. **Recommendation:**
   leave the typed wrappers alone; if a caller needs binary safety
   they reach for `cmd` directly. Two surfaces with clearly different
   purposes, not one surface with split personality.

2. **Should there be a `vk-key` newtype distinct from `vk-str`?**
   A separate Redis-key wrap would catch the (relatively rare)
   confusion between a key string and a value string. The c-dsl
   precedent did not split by role (one `CField` covers struct *and*
   union fields), and Redis itself does not type-discriminate
   key-vs-value at the protocol layer; splitting would be type-system
   noise. **Recommendation:** no.

3. **`Map (cstr cstr)` shorthand for `HSET many-fields`?** Redis's
   `HSET` accepts a flat alternating `field value` sequence; carrying
   it as a `Map` and unpacking at the call site would be nice but
   requires either a new spread operator or a helper that returns
   `Vec ValkeyArg`. Out of scope for this plan; revisit if HSET
   becomes a hot path.

## Rollback / fallback

The change is contained to `spices/valkey/`. If a follow-up surfaces
turmeric-side codegen edges (variadic + defdata + by-value carrier
through inline-C cons walk is not a shape this plan's author has
exercised end to end), rollback is `git revert` of the spice commit
and the `cmd` cons-list surface returns. No turmeric main change is
required by this plan; if one becomes necessary it would be filed as
its own report and this plan parked until the report lands.

## When to pick this up

Pick up P0 when:

- A downstream caller asks for binary-safe `cmd` (likely trigger), OR
- valkey's pipelined-reply U1 follow-up is touched (related surface,
  cheap to do together), OR
- nothing more compelling is open on Track C and we want to use a
  session to formally close U6's open follow-up rather than carry it.

Until one of those fires, the c-dsl-only U6 ship is the right
stopping point.
