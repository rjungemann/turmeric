# README documents a structural-equality operator =struct= that does not exist

**Severity: low** (docs advertise vaporware). Found in the 2026-08-20 docs
audit.
**Status: RESOLVED** -- README now shows the operator that exists.

## Repro

`grep -rn '=struct=' src stdlib tests` -> zero hits; `git log -S'=struct='` ->
never existed. `(println (=struct= [1 2 3] [1 2 3]))` cannot compile. The
README carried both an example block and a "Structural equality (`=struct=`)"
features row.

The example was wrong twice over: it also spelled its map literal `{:a 1}`,
which is SRFI-105 curly-infix arithmetic in every dialect, not a map.

## Resolution

Structural equality was never missing -- only its README spelling was. The
`Eq` typeclass already provides it on every container whose element type is
itself `Eq`, verified against `main`:

```lisp
(println (.eq? [1 2 3] [1 2 3]))        ; => true
(println (.eq? #map{:a 1} #map{:a 2}))  ; => false
(println (.eq? #set{1 2} #set{1 2}))    ; => true
```

So the example block was rewritten to the working form (with correct `#map{}`
/ `#set{}` literals) rather than deleted, and the features-table row now reads
"Structural equality (`Eq` typeclass / `.eq?`)". Nothing was implemented; the
✅ was accurate all along, against a name nobody could type.

## Verification

`tools/check-guide-pairs.py README.md` -- 10 pairs ok, 21 manifests ok. The
replacement snippet was run under `tur run` before being written down.

## Guides updated

- README.md (Structural equality example + features table row).
