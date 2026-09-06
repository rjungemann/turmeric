# A region node stored by an unhooked primitive still dangles after the rewind

**Severity: medium (silent wrong answer / segfault on the default build, only
for a program that stores a region node through a path the note does not
cover).** Filed 2026-09-06 during region-lock-hardening.

## What was fixed, and what this is the residue of

Regions (RM3, on by default since 2026-09-05) rewind a generation when the
bracket's RESULT cannot reach it. That was the whole check, and three programs
showed it is not enough: a node `vec-push!`ed into an outer vec from inside
`with-region`, a node behind an erased `:int` field of an admitted record
result, and a `(Vec int)` of erased nodes each segfaulted on the default build
and printed the right answer under `TUR_REGIONS=0`.

The fix (commit on 2026-09-06; `docs/archive/regions-plan.md`,
"region-lock-hardening") widens the runtime lock from the result word to every
word that leaves the generation: every store primitive the compiler knows about
says `TUR_REGION_NOTE(word)`, an erasing ascription notes the value it erases,
and a by-value aggregate result is noted by its words. The two fixtures
`region-escape-via-store` and `region-escape-via-erasure` read the stored
values back after the pop on both arms.

## What remains open

The note is only as complete as the set of stores that carry it. Two classes
do not, and cannot without more machinery:

1. **User inline-C.** A ` ```c ` body that writes a node pointer into memory
   it `malloc`ed itself -- the thread-argument cell in
   `tests/fixtures/thread-basic` is the shape -- is invisible to the emitter.
   A program that builds a node inside a bracket, stores it that way, and
   returns a scalar rewinds under it. This is `#fx{Unsafe}` territory by the
   language's own rules, and the fix direction is a documented contract, not
   a hook: an inline-C body that retains a caller's word past its own return
   must `TUR_REGION_NOTE(word)` it (the macro is always defined, `((void)0)`
   under `TUR_REGIONS=0`).

2. **Stdlib primitives outside the hooked set.** The hooked set is the one
   the gc-guide lists. Anything else in stdlib that stores a caller's word
   into heap memory -- the `sized-*-set!` family, `fiber-local-set!`,
   `save-cont!`, channel sends, scheduler queues, the serializer -- is not
   noted. Each is one line to hook when it is confirmed to store a word that
   can be a node; none has a repro today. The rule for a NEW primitive is in
   CLAUDE.md.

A third, latent, is the top-level panic jam: a panic caught by the outermost
`catch-unwind` after unwinding through a bracket leaves that generation open
with nothing left to pop it, so every later allocation lands in it (correct,
never freed until exit). Not reachable today -- both catch paths close the
bracket before the propagation check runs -- and a shallower pop now retires
skipped generations, so only the outermost case is left. The fix, if it is
ever needed, is for the catch boundary to record `tur_region_depth()` on entry
and retire down to it on the panic arm.

## Minimal repro (class 1)

```turmeric
(defdata Link :heap (Link [v : int nxt : int]))
(defn cell-new [] : ptr<void>
  ```c
  return malloc(sizeof(int64_t));
  ```)
(defn cell-set! [c : ptr<void> v : int] : nil
  ```c
  *(int64_t *)c = v;          /* unhooked store: no TUR_REGION_NOTE */
  ```)
(defn cell-get [c : ptr<void>] : int
  ```c
  return *(int64_t *)c;
  ```)
(defn main [] : int
  (let [c (cell-new)]
    (with-region (fn [] (do (cell-set! c (:: (Link 7 0) :int)) 1)))
    (println (cell-get c)))          ;; the address of a rewound node
  0)
```

Note that in THIS repro the erasing ascription `(:: (Link 7 0) :int)` fires
the erasure note and the bracket retires -- the typed variant (`cell-set!`
taking a `Link`) is the one that rewinds. Prefer the typed style everywhere
else; here it is what defeats the lock.

## Fix directions

- Document the inline-C contract (done in the gc-guide) and audit the stdlib
  `!`-suffixed primitives once, hooking any that store a word.
- Longer term, route the intermediaries a bracket creates (closure envs,
  element boxes) into the generation, so a node stored through one is
  region-to-region and the intermediary itself is what the note sees. That
  was RM3's original R2 direction and needs the free-side guard at every
  intermediary free site; it is not v1 work.
