# A field READ of a frame-named parameter downgrades `#writes` to UNVERIFIED

**Severity: medium.** Sound (it declines, never miscompiles), but it defeats
WF2 for most real bodies and silently disables every consumer that requires
`writes_checked` -- notably WF3's borrow widening. Found 2026-08-22 while
building the `write-frames` graduation-shim fixture.

## Summary

Reading a struct field of a parameter the frame names -- `(.n a)` -- makes the
WF2 walk answer `UNVERIFIED` for the whole function. A read is not a write
channel, so the frame should still verify. Because the downgrade is silent by
design (UNVERIFIED carries no diagnostic), a `#writes` frame on any body that
touches a field of its own parameter quietly documents intent and nothing more.

## Minimal repro

```turmeric
(defstruct Ctr [n : int])

;; identical bodies except for the trailing field read
(defn no-read   [^mut a : Ctr] #writes [a] : int (set! (.n a) 1) 0)
(defn with-read [^mut a : Ctr] #writes [a] : int (set! (.n a) 1) (.n a))

;; a field read of a LOCAL is fine -- not parameter-rooted
(defn local-read [^mut a : Ctr] #writes [a] : int (let [t (Ctr 3)] (.n t)))

(defn main [] : int (println 0) 0)
```

```
$ tur run --dump-write-frames repro.tur
write-frame no-read:    VERIFIED   mask=0x1 frame=VERIFIED   global=NO
write-frame with-read:  UNVERIFIED mask=0x1 frame=UNVERIFIED global=NO   <-- wrong
write-frame local-read: VERIFIED   mask=0x1 frame=VERIFIED   global=NO
```

The empty frame is hit the same way, which is worse because `#writes []` is the
frame WF3 most wants: `(defn peek [^borrow a : Ctr] #writes [] : int (.n a))`
answers UNVERIFIED. `tests/fixtures/wf1-writes-frame-honored` already contains
exactly that `peek` and its header says "compiles clean (WF2 verifies the
frame)" -- true for the fixture as a whole, but not for `peek`, which is
silently unverified. Nothing caught it because the fixture does not pass
`--dump-write-frames`.

## Root cause

`wf_walk`, `src/compiler/elab_fns.c:1560-1579`.

The walk treats any `F_LIST` with a symbol head as a possible call. A field
access `(.n a)` is such a form: head symbol `.n`, argument `a`, which is
parameter-rooted, so `passes_param` becomes true (`elab_fns.c:1567-1573`). It
then resolves the head as a callee:

```c
Binding *callee = scope_lookup(&e->global, head);
if (!callee) {
    /* Not resolvable here (a local fn value, a typeclass method, a
     * builtin): no frame to consult, so no vouching. */
    v = wf_worse(v, WF_UNVERIFIED);
}
```

`.n` is a field accessor, not a global binding, so `scope_lookup` fails and the
body is downgraded on the "unresolvable callee" branch -- a branch written for
opaque *calls*, reached here by a read that cannot write anything.

## Fix directions

Recognize a field access before the callee analysis and let it fall through to
the ordinary recursive descent at `elab_fns.c:1644-1646`, which already visits
the operand. A field read has no write channel at all, so it needs no vouching:
neither channel 2 (parameter mode) nor channel 3 (callee frame) has a subject.

Whatever predicate the elaborator already uses to recognize an accessor head is
the one to reuse here rather than matching a leading `.` in `wf_walk`. The
write direction is unaffected: `(set! (.n a) 1)` is handled by the `set!` place
channel above this code, not by the call analysis.

Note the blast radius is verdict-widening, so it will move fixtures that
currently pin UNVERIFIED (`read-frames-dump-verdicts`,
`g1-writes-global-unverified`, and the `peek` line above). Each needs reading
individually: a fixture that pins UNVERIFIED *because the walk cannot see a
callee* is still correct, while one that pins it because of a field read is
pinning this bug.
