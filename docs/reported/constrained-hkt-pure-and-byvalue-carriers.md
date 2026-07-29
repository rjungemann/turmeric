---
status: open
severity: medium
discovered: 2026-07-29
area: compiler (constrained HKT dispatch, src/compiler/elab_typeclasses.c)
---

# Constrained HKT polymorphism cannot call `pure`, and rejects by-value carriers

## Summary

Mode-B dictionary passing works: a constrained kind-polymorphic function
(`[^m] [^Monad m x : (m int)]`) is compiled once and dispatches through a
dictionary the caller resolves. Two gaps keep it from being usable for ordinary
monadic code:

1. **Return-position methods are not dictionary-backed.** `pure` (and `empty`)
   on the abstract `m` fails with `no instance 'Applicative tyvar'` even when
   `^Applicative m` is declared. Receiver-directed methods (`bind`, `fmap`)
   resolve fine.
2. **The poly carrier is one machine word.** The abstract `m` must be an
   int-carrier `defopaque`; a by-value ADT carrier (stdlib `Option`, `Result`)
   segfaults through it.

Gap 1 is the blocking one. Almost every monadic combinator worth abstracting
ends in `pure`, so the feature currently expresses `f a -> f a` transformations
but not `a -> m a` construction. Gap 2 means the two monads a user is most
likely to reach for cannot be the abstract `m` even for the shapes that do work.

Found while documenting the boundary in `docs/guides/effects-vs-monads.md`.

## What already works (do not regress this)

    (defopaque Id [a] :int)
    (defn mk-id [A] [x : A] : (Id A) ```c return (int64_t)x; ```)
    (defn un-id [A] [b : (Id A)] : A ```c return (int64_t)b; ```)
    (definstance Monad [Id] (bind [ma k] (k (un-id ma))))

    (defopaque Halt [a] :int)
    (defn mk-halt [A] [x : A] : (Halt A) ```c return (int64_t)x; ```)
    (defn un-halt [A] [h : (Halt A)] : A ```c return (int64_t)h; ```)
    (definstance Monad [Halt] (bind [ma k] ma))

    (defn double-it [^m] [^Monad m x : (m int)] : (m int)
      (bind x (fn [v] (mk-id (* v 2)))))

    (defn at-id   [g (forall [(m :: * -> *)] [(Monad m)] (-> (m int) (m int)))] : int
      (un-id   (g (mk-id 5))))
    (defn at-halt [g (forall [(m :: * -> *)] [(Monad m)] (-> (m int) (m int)))] : int
      (un-halt (g (mk-halt 5))))
    ;; => 10, then 5.  One body, caller-chosen instance.

Direct calls work too -- `(un-id (double-it (mk-id 5)))` gives `10` without the
rank-2 hop. `tests/fixtures/forall-dict-fmap` pins the `Functor` equivalent.

## Gap 1 repro -- `pure` on the abstract `m`

    $ cat > /tmp/p.tur <<'EOF'
    (defopaque Box [a] :int)
    (defn mk-box [A] [x : A] : (Box A) ```c return (int64_t)x; ```)
    (defn un-box [A] [b : (Box A)] : A ```c return (int64_t)b; ```)
    (definstance Monad [Box] (bind [ma k] (k (un-box ma))))
    (definstance Applicative [Box] (pure [x] (mk-box x)) (ap [ff fa] fa))
    (defn poly [^m] [^Monad m ^Applicative m x : (m int)] : (m int)
      (bind x (fn [v] (pure (* v 2)))))
    (defn main [] : int (println (un-box (poly (mk-box 5)))) 0)
    EOF
    $ ./build/tur run /tmp/p.tur
    /tmp/p.tur:8:19: error: no instance 'Applicative tyvar'

Expected: `10`. Replacing `(pure (* v 2))` with the concrete `(mk-box (* v 2))`
compiles and prints `10`, which localizes the failure to `pure` alone -- the
`Applicative Box` instance exists, is declared in the constraint set, and is
still not consulted.

The same error appears with the constraint omitted, i.e. declaring
`^Applicative m` changes nothing -- the constraint is parsed but the
return-position dispatch path never reaches for it.

## Gap 2 repro -- by-value carrier

    $ cat > /tmp/w.tur <<'EOF'
    (defn dbl [v : int] : int (* v 2))
    (defn poly-bind [^m] [^Monad m x : (m int)] : (m int)
      (bind x (fn [v] (some (dbl v)))))
    (defn use-opt [g (forall [(m :: * -> *)] [(Monad m)] (-> (m int) (m int)))] : int
      (unwrap-or (g (some 5)) -1))
    (defn main [] : int (println (use-opt poly-bind)) 0)
    EOF
    $ ./build/tur run /tmp/w.tur
    Segmentation fault

Expected: `10`. The identical program with an int-carrier `defopaque` in place
of `Option` prints `10`.

Note this is the *general* constrained-poly path. The van Laarhoven **lens**
boundary already got a fix for wide by-value functors -- see
`docs/archive/history/van-laarhoven-functor-must-be-int-carrier.md`, resolved
2026-07-04 by boxing at the lens crossings (Path A), with a zero-overhead
monomorphizing Path B under `--enable=vl-wide-mono`. That work did not
generalize to plain constrained calls, and the boxing it introduced is the
obvious thing to reuse here.

## Also: the middle-vector spelling miscompiles

`(defn f [^m] [(Monad m)] [ma : (m int)] ...)` -- the `definstance`-style
constraint vector rather than the in-parameter `^Monad m` -- elaborates and
reaches monomorphization, then emits bad C:

    error: incompatible types when assigning to type 'int64_t' from type 'tur_adt_Option__int'
    (in dup__spec__tur_adt_Option__int_tur_adt_Option__int)

Two accepted spellings for the same thing, only one of which works, is worth
resolving one way or the other -- either make the middle vector work or reject
it with a diagnostic pointing at `^Monad m`.

## Fix directions

1. **Gap 1 is the priority.** Return-position dispatch currently resolves from
   the *expected type* (see the resolved
   `docs/archive/history/return-directed-methods-pure-empty-inference.md`). When
   the expected type is an abstract constrained tyvar, that lookup should fall
   through to the constraint set and emit a dictionary load against the ambient
   dict binding -- `e->cur_hkt_dict_binding` is already threaded for the
   receiver-directed case in `elab_call.c:5817-5845`, so the plumbing exists.
2. **Gap 2**: reuse the van Laarhoven Path A boxing at the general poly-carrier
   crossing, or extend the `__spec__` monomorphization so a by-value carrier is
   specialized rather than squeezed through the word-sized carrier.
3. Fixtures for both, alongside `forall-dict-fmap`: a `Monad`-constrained body
   that calls `pure`, and one instantiated at a by-value ADT carrier.

Until gap 1 lands, `effects-vs-monads.md` documents the feature as expressing
`m a -> m a` transformations only, and steers readers to concrete monads or the
effect formulation.
