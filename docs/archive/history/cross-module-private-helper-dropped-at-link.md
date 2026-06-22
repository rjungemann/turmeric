# Fix paper trail -- cross-module private helper "dropped at link"

Resolved 2026-06-22. **Misdiagnosed report -- no turmeric change required.**

## Original claim

Three `frame` spice tests (`reshape_test`, `group_test`, `interop_test`) failed
at the C link step with `undefined reference to frame__sort____so_take` (and
`frame__interop____ip_*`). The report attributed this to a DCE / separate-
compilation retention bug: "a private (`__`-prefixed) helper that *is*
referenced gets dead-code-eliminated / not emitted," in the lineage of #465/#467.

## Investigation

Reproduced with `tur run tests/frame/reshape_test.tur` (built `tur` from this
branch) against a fresh clone of turmeric-spices. Inspected the emitted C
(`tur emit-c`):

- `frame__sort____so_take` appears **only** as the extern declaration + call
  inside the two bridges -- never as a definition.
- `frame/sort/__so-take` **is** emitted, as a real definition:
  `static int64_t frame__sort___un_unso_hytake(int64_t, int64_t, int64_t) { ... }`,
  and sort.tur's own callers use that name. So the helper is NOT dropped/DCE'd.

The two names differ because of the name mangler, not retention. The mangler
(`src/compiler/mangle.c:11-21`) is injective and self-delimiting:

- `/` (module separator) -> `__`
- literal `_` -> `_un`
- `-` -> `_hy`

So `frame/sort/__so-take` -> `frame__sort__` + `_un_unso_hytake`
= `frame__sort___un_unso_hytake`. The bridge's hand-spelled
`frame__sort____so_take` assumes a leading `__` passes through and `-` -> `_`,
which is **not** this mangler's scheme (mangle.c last changed in #457, before
#467 -- it has not regressed). The bridge simply calls a symbol that was never
emitted under the current (and #467-era) scheme.

`#[used]` does not help: I added it to `__so-take` and the link still failed --
because the defect is the *name*, not the linkage. `#[used]` would only matter
once the name matched and the build crossed a TU boundary.

The `frame/interop` `__ip_*` failures are the same class: hand-spelled mangled
names for Arrow C Data Interface `release` callbacks taken by address.

## Why it is spice-side

turmeric's mechanisms for exactly this situation are intact:

- `#[used]` (#467) keeps external C linkage for a defn reached only via a raw
  `extern`.
- The `__TUR_CNAME_<source-name>__` splice reproduces the live mangler so
  inline-C never hand-spells a mangled name. The c-integration guide explicitly
  recommends it over hand-spelling.

The clean, idiomatic fix is to stop bridging to a private helper through a raw
`extern` and instead export it and call it normally. Verified locally (not
pushed -- turmeric-spices is a separate repo, out of this branch's scope):

```turmeric
;; frame/sort.tur
(defmodule frame/sort
  (export arrange arrange-indices reorder __so-take)   ;; + __so-take
  ...)

;; frame/group.tur and frame/filter.tur
(import frame/sort :refer [arrange-indices reorder __so-take])
;; bridge body becomes a normal call:
(defn __so-take-bridge [col : int perm : int n : int] : int
  (__so-take col perm n))
```

Result against this branch's `tur`:

```
tur run tests/frame/reshape_test.tur   => 1..12   (all ok)
tur run tests/frame/group_test.tur     => 1..7    (all ok)
```

(`interop_test` wants the same treatment for its `__ip_*` by-address callbacks:
`#[used]` + a `__TUR_CNAME_` address-of, or a re-export.)

## Turmeric-side outcome

No compiler change. The report's "bisect turmeric main to find where
`__so_take` stopped being emitted" premise is moot -- it was never not-emitted;
it is emitted under the name the live mangler produces. Resolved as misdiagnosed
and re-pointed at the spice.

### Optional future ergonomic (noted, not implemented)

The `__TUR_CNAME_` splice resolves only names in scope, so a cross-module bridge
to an *unexported* sibling-module helper cannot use it (the export+normal-call
path above is the idiomatic answer). A qualified splice form
(`__TUR_CNAME_frame/sort/__so-take__`) that resolves an unexported sibling
symbol would let such bridges avoid hand-spelling without an export. Speculative
enhancement, not a bug.
