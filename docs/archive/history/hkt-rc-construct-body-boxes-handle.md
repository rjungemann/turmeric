---
status: resolved
severity: high
discovered: 2026-07-26
resolved: 2026-07-26
area: compiler (HKT by-value instance bodies, emit)
---

# An HKT instance body that CONSTRUCTS an rc heap-boxes the handle, and the caller reads the box

## Summary

A pure-Turmeric `Functor [rc]` instance whose body builds a fresh `rc` returns
one indirection too many. The body mallocs a cell, stores the `RcControlBlock *`
into it, and returns the *cell*:

    { RcControlBlock * *__tur_ret_p = (RcControlBlock * *)malloc(sizeof(RcControlBlock *));
      *__tur_ret_p = __t45;
      return (int64_t)(intptr_t)__tur_ret_p; }

while the dispatch consumer reads that value as the handle itself:

    RcControlBlock * m_1352 = (RcControlBlock *)(intptr_t)(__ps_174);

Everything downstream then operates on a pointer-to-pointer as if it were a
control block. No diagnostic, no warning -- it compiles clean and produces
garbage.

## Resolution (2026-07-26)

Fixed via fix direction 1 -- stop boxing, in one place.

The culprit is the `inst_method_carrier_spill` return path in
`src/compiler/emit_fns.c`. It exists for a real reason: the dispatch dict's slot
is a uniform `int64_t (*)(...)`, so an instance method returning a **by-value
aggregate** has to spill it to the heap and hand back the pointer. It was firing
for *any* carrier-ABI result, including one that is already carrier-width.

The same double-box had already been found and fixed one branch earlier, for an
`int64_t` spill type -- the comment there describes the `(Result _ E)` fmap
returning 0 for 42, which is the identical failure. A pointer is carrier-width
for exactly the same reason, so the guard now covers both:

    bool spill_ty_is_ptr = struct_cty && strchr(struct_cty, '*') != NULL;
    if (struct_cty && (strcmp(struct_cty, "int64_t") == 0 || spill_ty_is_ptr))
        buf_printf(file, "return (int64_t)(intptr_t)%s;\n", ret_val);

The general pointer rule rather than an rc-specific one, because the very next
branch already treats a TY_RC / TY_WEAK / TY_REF / TY_LREF body returned through
the int64 carrier this exact way -- a bare `(int64_t)(intptr_t)` bridge. The
spill was intercepting a case that was already handled correctly downstream.
Suite: 2371 passed, 0 failed, zero snapshot churn.

`stdlib/rc.tur`'s `Functor [rc]` is now ordinary Turmeric:

    (definstance Functor [rc]
      (fmap [container g] (rc/of (g (rc-payload container)))))

The suspected root cause below was close but not exact: the disagreement is not
between `m7_byvalue_grounded` and the collapsed result type, it is the spill
firing on a width test it never made. The `rc_cb_alloc(0, 36, ...)` observation
was a red herring -- 36 is the element tyvar's ordinal, which only means the
block is conservatively marked `may_contain_cycles`; it is not part of this bug
and is unchanged by the fix.

Pinned by `tests/fixtures/hkt-instance-rc-construct-result`, which asserts values
and counts rather than "it compiles" -- the broken form compiled clean with no
warning. Verified to genuinely catch the regression: with the fix reverted it
reports a garbage strong-count, a fold of 0, and 752768 bytes leaked over 5000
iterations.

The original report follows for the record.

## Repro

    (load "stdlib/rc.tur")

    (definstance Functor [rc]
      (fmap [container g] (rc/of (g (rc-payload container)))))

    (defn dbl [x : int] : int (* x 2))
    (defn add [a : int b : int] : int (+ a b))

    (defn main [] : int
      (let [r (rc/of 21)]
        (let [m (fmap r dbl)]
          (println (rc/strong-count m))        ; expect 1
          (println (:: (foldl m 0 add) int)))) ; expect 42
      0)

Observed: `94022973637440` and `0`. Expected `1` and `42`. A 5000-iteration
fmap/drop loop leaks 752768 bytes, because `rc/drop` decrements the box rather
than the block.

The same shape written as inline-C (returning `(int64_t)(intptr_t)out` directly)
is correct -- that is what `stdlib/rc.tur` ships, with a comment pointing here.

## Scope

Only the **construction** path. `Foldable [rc]`'s bodies are pure Turmeric and
correct, because they only READ the receiver:

    (definstance Foldable [rc]
      (foldl [ta init f] (f init (rc-payload ta)))
      (foldr [ta init f] (f (rc-payload ta) init)))

So the trigger is an HKT instance body that *builds* a value of a pointer-family
family type and returns it through the method's applied `(f b)` result.

## Newly reachable, not newly broken

This path could not be written before 2026-07-26: an instance body saw its own
receiver as the applied `(t a)`, which unified with nothing, so there was no way
to get at the payload without inline-C
(`docs/archive/hkt-fmap-result-is-not-droppable.md`). The parameter-side collapse
that unblocked the pure-Turmeric `Foldable` also makes this construction shape
expressible -- and it miscompiles.

That makes it a **latent trap for user code**: a spice author writing the obvious
`Functor [rc]` instance gets silent corruption, not an error.

## Root cause (suspected, not confirmed)

The boxing looks like the return path for a method whose declared result is the
applied `(f b)` -- an aggregate/by-value shape -- taking an "indirect return"
route, while the collapsed `rc<int>` result type at the call site says "this is a
plain handle". The two halves disagree about indirection, the same class of
carrier-vs-by-value mismatch documented around
`m7_byvalue_grounded` in `elab_typeclasses.c`.

Worth checking first:

- Does `m7_body_byvalue_ok` flip to true for the pure-Turmeric body (it does --
  it is `body->kind != EX_INLINE_C` plus `m7_body_constructs_byvalue`), and does
  the ptr-family result collapse then commit a result type that disagrees with
  the by-value spec the producer emits?
- The value type ordinal also differs: the inline-C body passes
  `rc_cb_alloc(0, 3 /* TY_INT */, NULL)`, the generated body passes
  `rc_cb_alloc(0, 36, NULL)` -- 36 is not a primitive ordinal, so the block is
  additionally marked `may_contain_cycles`. Benign on its own, but it shows the
  element type is not grounding to `int` in the body either.

## Fix directions

1. Make the construction path agree on indirection: either stop boxing when the
   family head is pointer-family (the handle IS the carrier), or teach the
   consumer to unbox. The first is almost certainly right -- boxing an
   already-pointer-width handle buys nothing.
2. Failing that, **reject** the shape with a diagnostic rather than
   miscompiling it. A `definstance` over a pointer-family head whose body
   constructs is currently a silent corruption; an error naming the limitation
   would be strictly better.
3. Pin with a fixture asserting `rc/strong-count` and a fold over an
   `fmap` result built by a pure-Turmeric body.

## Related

- `docs/archive/hkt-fmap-result-is-not-droppable.md` -- the result-side collapse
- `docs/reported/hkt-inline-c-instance-body-loses-result-type.md` -- the general
  inline-C gap
- `stdlib/rc.tur` -- carries the inline-C workaround and a pointer here
