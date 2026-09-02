# defimage-global registry + TUR-W0706 lint (plan AI3) unimplemented -- mutable globals silently fall out of image dumps

**Severity: medium** (RESOLVED 2026-09-02) -- a silent-data-loss foot-gun the guide already warns
about: the workaround is threading state through the captured continuation.
Found in the 2026-08-20 docs audit.

## Repro

`grep -rn "defimage-global\|W0706" src/ stdlib/` -> nothing. A `def ^mut`
written during init is absent after a warm `load-image!`.

## Root cause / tracking

docs/archive/history/application-image-dumps-plan.md phase AI3, unbuilt.

## Fix direction

Per the plan: a registration form that serializes declared globals alongside
the continuation in the TSER payload, plus the lint for unregistered mutation
reachable from a cache body.

## Guides to update when fixed

- docs/guides/image-dumps-guide.md ("Globals" section and See-also)

## Resolution (2026-09-02)

Plan AI3 is built, in `stdlib/image.tur`:

- `(defimage-global name :T initial)` is `(def ^mut name :T initial)` plus
  `name/image-ser`, `name/image-deser` (the global's `Serializable` instance
  for `T`, monomorphic -- the return-type ascription `(:: (deserialize b) :T)`
  selects it) and `name/image-track!`, which registers the pair by name in
  `image/global-registry`. `(image/track-globals! a b ...)` at the top of
  `main` installs them on both cold and warm starts; the hooks' rule, for the
  hooks' reason (compiled top-level forms do not execute).
- `image/save-cont-to-file!` snapshots the registry (`image/snapshot-globals`)
  and `image/write-image-file!` writes it as a second section after the
  continuation bytes -- `[i64 len][i64 n]{[i64 name_len][name][i64 blen]
  [bytes]}` -- located by the header's previously reserved `globals_offset`
  (byte 56) and counted in `payload_len`. `image/load-resume-file!` now reads
  the whole payload (`image/read-image-file`), restores the section
  (`image/restore-globals!`) and only then deserialises and resumes the
  continuation, so the resumed tail reads the post-init values. A zero
  `globals_offset` reads exactly as before.
- `TUR-W0706` (`wf_lint_image_globals`, src/compiler/elab_fns.c): the
  `with-image-cache-after-init` expansion notes its `init` root
  (src/compiler/elab_macros.c); after the whole unit is elaborated the G1
  global-write walk (`wf_fn_writes_global`, transitive over named callees)
  lists the globals `init` writes and warns for each with no
  `<name>/image-deser` binding in global scope. `loop` is not linted (it runs
  on both paths) and neither is the closure-taking `with-image-cache`.

Found on the way: `stdlib/serial.tur`'s `Serializable [float]` `deserialize`
returned the bit pattern through an `int64_t` lvalue from a function the
emitter types as returning `double`, so every float deserialize came back
numerically converted (`7.1` -> `4.61968e+18`). It returns the double now;
no fixture had round-tripped a float through `deserialize` before.

Pinned by `tests/fixtures/image-globals-roundtrip` (int / cstr / float
globals written in `init`, clobbered between the cold and warm calls,
observed at their post-init values after the warm resume; 3 registered) and
`tests/fixtures/warn-image-global-unregistered` (one warning, for the plain
global reached through a callee; the declared one is silent; the program
still runs). Guide "Globals" section rewritten; plan header updated.
