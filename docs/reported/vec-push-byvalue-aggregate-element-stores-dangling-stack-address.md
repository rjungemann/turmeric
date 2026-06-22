# pushing a by-value aggregate element into a heap container stores a dangling stack address

Repo: rjungemann/turmeric
Found by: the vec-push! carrier-cast fix
  (docs/archive/vec-push-heap-struct-element-not-carrier-cast.md) -- this is the
  lifetime caveat noted there, filed as its own finding.
Severity: Medium. Latent undefined behavior: a by-value aggregate element pushed
  into a `Vec` (or any heap container) that outlives the enclosing expression
  stores the address of a stack temporary. Works by luck today (the stack slot
  is often still resident when read back); a different stack layout, `-O2`, an
  intervening deep call, or a threaded reader can read freed-frame garbage.

## Summary

When a by-value aggregate (e.g. `(Option int)` lowered to a by-value
`Option__int` struct) crosses the int64 element carrier into a heap container,
the concrete->carrier bridge spills the value to a fresh local and stores
`(int64_t)(intptr_t)(&tmp)`. For an *immediate* carrier consumer (dictionary
dispatch, a same-expression call) the temp is live and this is correct. For a
`Vec` that escapes -- returned, stored in a field, captured -- the stored
pointer dangles the moment the frame holding `tmp` returns.

## Repro

    (defn build-vec [] : int
      (let [vo (:: (vec-new) (Vec (Option int)))]
        (vec-push! vo (:: (some 5)  (Option int)))
        (vec-push! vo (:: (some 99) (Option int)))
        (:: vo :int)))                 ;; the Vec escapes build-vec's frame

    (defn main [] : int
      (let [vo (:: (build-vec) (Vec (Option int)))]
        (let [a (:: (vec-get vo 0) (Option int))
              b (:: (vec-get vo 1) (Option int))]
          (println (if (.is-some a) (.value a) -1))    ;; wants 5
          (println (if (.is-some b) (.value b) -1)))   ;; wants 99
        0))

Prints `5` / `99` today, but only by luck. The emitted `build-vec` is:

    static int64_t build_hyvec() {
        Vec__Option__int * vo_1024 = vec_new__spec__Vec__Option__int__();
        Option__int __t45 = some__spec__Option__int_int64_t(INT64_C(5));
        vec_hypush_ex(..., (int64_t)(intptr_t)(&__t45));   // address of a local
        Option__int __t46 = some__spec__Option__int_int64_t(INT64_C(99));
        vec_hypush_ex(..., (int64_t)(intptr_t)(&__t46));   // address of a local
        ...
        return (int64_t)(intptr_t)(vo_1024);               // Vec holds &__t45,&__t46
    }                                                       // __t45/__t46 die here

`vo` returned to `main` holds pointers into `build_hyvec`'s reclaimed frame.

## Root cause

`emit_carrier_bridge`, CK_CONCRETE -> CK_CARRIER, aggregate branch
(`src/compiler/emit_core.c:3192-3200`):

    /* Aggregate: spill to a local, return its address as int64_t.
     * The spill local uses a fresh tmp so it stays live through the
     * expression that consumes the carrier value. */
    char *tmp = fresh_tmp(ctx);
    buf_printf(body, "%s %s = %s;\n", cname, tmp, src_str);
    buf_printf(&out, "(int64_t)(intptr_t)(&%s)", tmp);

The "stays live through the expression" assumption holds for an immediate
consumer but not for a heap container that stores the carrier and outlives the
expression. The new push-side bridge
(`src/compiler/emit_expr.c`, "vec-push-heap-struct-element-not-carrier-cast")
routes a by-value aggregate element through exactly this branch, so a
`(Vec <by-value-aggregate>)` build now compiles cleanly but inherits the
dangling-address hazard. (Heap-pointer elements -- `(Vec (Vec int))`,
`(Vec (Cons int))` -- are unaffected: their carrier IS the heap pointer, shared
by identity, no spill.)

## Fix directions

The element value must outlive the container, not the expression. Options, in
rough order of preference:

1. **Heap-promote the element** when bridging concrete->carrier for a value
   that flows into a heap-container insert: `malloc` a copy and store the heap
   pointer as the carrier (matching how `:heap` structs are already carried),
   instead of `&tmp`. Needs the bridge to know the sink is a container insert
   (or to always heap-promote aggregates crossing into the carrier, accepting
   the allocation cost).
2. **Carry by-value aggregate vec elements inline** -- store the aggregate in
   a wider element slot rather than the int64 carrier, so no address is taken.
   Larger codegen change to the Vec representation for non-pointer elements.
3. **Reject / diagnose** pushing a by-value aggregate whose carrier would be a
   stack address into an escaping container, until 1 or 2 lands -- safer than
   silently emitting UB.

A regression test should read the element back *after* the producing frame has
returned and been clobbered (the repro above, plus an intervening deep call),
ideally under ASan `detect_stack_use_after_return=1`.
