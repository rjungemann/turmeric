# `set-add-elem__` adds elements that `set-member?` cannot find

**Severity: medium.** A silent wrong answer at the API level: the element IS in
the set (`set-count` counts it) but every membership test says `false`. Nothing
crashes, so a caller mixing the two spellings gets a set that quietly behaves
as if empty.

**Status:** open. Found 2026-09-11 building `spices/crdt`'s G-Set on
crdt-spice-plan C2.

## Repro

```turmeric
(defn main [] : int
  (let [a  (set-add-elem__ (set-new) (quote x))
        a2 (set-add-elem__ a (quote y))]
    (println (set-count a2))                                ;; 2  -- both added
    (println (set-member? a2 (hash (quote x)) (quote x))))  ;; false -- WRONG
  0)
```

The same set built through the `set-add` macro is fine:

```turmeric
(let [s (set-add (set-new) (hash (quote x)) (quote x))]
  (println (set-count s))                              ;; 1
  (println (set-member? s (hash (quote x)) (quote x))))  ;; true
```

## Mechanism

`set-add` and `set-member?` (`stdlib/set.tur:127`, `:173`) are macros taking an
explicit hash and forwarding it to `set-add-eq-o` / `set-has-eq-o?` alongside
`mk-box` / `mk-cmp` / `mk-owned?`. Given the same `h` on both sides they agree,
which is what the second snippet shows.

`set-add-elem__` (`:524`) is the typed `[^Hash A ^MapKey A]` entry point and
computes the hash itself. Whatever it computes is not what `(hash x)` returns
for the same element, so a set built with it cannot be queried with the macro.

Which one is wrong is the open question. If `set-add-elem__`'s internal hash is
the intended one, then `set-member?` needs a typed twin that computes the hash
the same way -- there is `set-add1` (`:545`, sugar for `set-add-elem__`) but no
`set-member1`, which is probably how the gap survived.

## Why it bit here

A G-Set's entire content is "add elements, union two sets, ask whether an
element is present". The typed adder is the one a reader reaches for first --
it needs no hash argument and has the `Hash`/`MapKey` constraints that suggest
it is the principled entry point.

`spices/crdt` uses the explicit-hash `set-add` / `set-member?` pair throughout
as a result, with a comment pointing here.

## Fixture owed

The repro above, asserting `true`. Note that `set-count` alone passes either
way -- the assertion has to be a membership test, which is why a fixture that
only checked sizes would not have caught this.
