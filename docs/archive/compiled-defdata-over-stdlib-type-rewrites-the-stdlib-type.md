# A compiled `defdata` reusing a stdlib type name rewrites the stdlib's own type

**Severity:** low-medium. A user program that picks a stdlib type name
(`Option`, `Result`, ...) for its own `defdata` gets errors blamed on the
stdlib's source files, with no mention of the name clash.

**Status:** OPEN. Filed 2026-09-16 while executing
[playground-session-hygiene-plan](../archive/playground-session-hygiene-plan.md)
(PS4), which refuses this in interpreter sessions but deliberately did not
change the compiled path.

## Repro

```turmeric
(defdata Option (Nada) (Algo int))
(defn main [] : int (println (match (Algo 3) (Nada) 0 (Algo n) n)) 0)
```

`tur run` reports, against the stdlib rather than the program:

```
stdlib/option.tur:233:1: error [TUR-E0012]: kind mismatch (TUR-E0012): instance of 'Functor' provides a kind-'*' type for parameter 1 which expects kind '* -> *'
stdlib/option.tur:243:1: error [TUR-E0012]: ... 'Applicative' ...
```

`defstruct` with a stdlib name is refused cleanly ("already defined by an
auto-loaded stdlib module", fixture `errors/defstruct-redef-stdlib-name`);
`defdata` is not.

## Root cause

`elab_defdata` (`src/compiler/elab_structs.c`) treats an existing type binding
as a forward stub whenever `elab_is_forward_type(e, name)` holds. The stdlib's
own type pre-pass registered `Option` as a forward type, so the user's
`defdata Option` passes that test after the stdlib has FILLED the stub, and
re-elaborates over it in place -- the stdlib's `Functor`/`Applicative`
instances then see a kind-`*` type. `defstruct` has a guard for exactly this
(the `prior_fully_defined` scan in `elab_defstruct`); `defdata` never got one.

## Fix directions

Mirror the `defstruct` guard in `elab_defdata`: an existing ADT with
`n_ctors > 0` is not a forward stub. In a compile it is a redefinition error
naming the stdlib when `elab_file_is_stdlib(def->origin_file_id)`; in an
interpreter session PS4 already sends earlier-turn user types down the reuse
path and refuses stdlib ones, so the new check must keep that exception.

## Resolution (2026-09-19)

Fixed in `elab_defdata` (`src/compiler/elab_structs.c`), mirroring the
`defstruct` guard exactly as the fix direction said: a def with `n_ctors > 0`
is FILLED, not a forward stub, whatever `elab_is_forward_type` says about the
name.

- A filled def whose origin file is under `stdlib/` is a redefinition error,
  in a compile and in a session alike:

  ```
  p.tur:1:10: error: defdata: 'Option' is already defined by an auto-loaded
  stdlib module, and redefining it would change it for the stdlib code built
  on it; pick a distinct name
  ```

  The PS4 refusal was already there for sessions, gated on
  `elab_prior_turn_adt`, which is false for a compile -- that gate was the
  whole bug. The stdlib arm no longer asks it.
- A filled def from THIS compile (the same-file `defdata Shape ... defdata
  Shape`) is "already defined by an earlier form", as for `defstruct`. It used
  to pass the forward-stub test too and silently re-elaborate over the first
  definition.
- A filled def from an EARLIER session turn keeps PS4's reuse path: that is
  the one shape that legitimately re-elaborates over its binding, and it is
  exactly what `elab_prior_turn_adt` was distinguishing.

Pinned by `tests/fixtures/errors/defdata-redef-stdlib-name` (the repro above)
and `tests/fixtures/errors/defdata-redef-earlier-form`;
`errors/defstruct-redef-stdlib-name` is unchanged.
