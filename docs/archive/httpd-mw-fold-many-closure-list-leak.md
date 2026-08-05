# httpd-mw-fold-many: runtime-built closure list leaks (onion + spine + factories)

> **RESOLVED (2026-07-22, closure-drop-glue R3a).** Leak-clean flag-on: `mw-count`
> annotated `^fat next` (onion walkable) + two new stdlib teardown primitives the
> fixture calls -- `httpd-mw-drop` (drops the composed onion) and
> `httpd-mw-free-chain` (drops each head factory + frees each spine cell of the
> input list). Marker dropped, `flags: --enable=closure-drop-glue` added, suite
> green. The *automatic* auto-drop (honest `Closure` / `list<Closure>` typing off
> `:int`) remains future work -- see R3a in
> `docs/upcoming/closure-drop-glue-plan.md`. Original finding below.

**Summary:** `httpd-mw-fold-many` leaks ~4.5 KB / ~211 allocs flag-on
(`--enable=closure-drop-glue`) -- the compose-middleware onion "at scale", built
from a RUNTIME cons list rather than a variadic-rest call, so none of the landed
Model R paths reach it.

**Severity:** Low (bounded, process-lifetime; the fixture keeps
`requires.no-leak-check`). Relevant to the plan's R2/R3, not a correctness bug.

**Repro:** `tur --enable=closure-drop-glue build
tests/fixtures/httpd-mw-fold-many/input.tur` then run under LSan. The fixture:

```
(defn mw-count [next : int] : ptr<void> ...)          ; next : int (not ^fat)
(defn make-mw [] : ptr<void> (let [_d ...] (fn [n : int] (mw-count (+ n _d)))))
(defn build-chain [n acc] ... (build-chain (- n 1) (tcons (:: (make-mw) int) acc)))
(let [chain    (build-chain 200 nil)
      composed (httpd-mw-fold (:: base int) chain)]
  (httpd-call composed 0))
```

**Root cause (three distinct sources, by allocator frame):**
1. `mw_hycount` -- the wrapper onion. `mw-count`'s `next : int` is not `^fat`, so
   the drop-glue walk does not free the chain. AND `composed` (the fold result)
   is a bare `:int` let-binding that nothing frees at scope exit -- there is no
   `httpd-free` here (no server), so even the outer box leaks.
2. `ctor_Cons__int` -- the `chain` cons spine (~200 cells from `build-chain`'s
   `tcons`), consumed by the fold and never freed.
3. `make_hymw` -- the ~200 factory closures, each applied once by
   `httpd-mw-fold` and discarded.

**Why the landed fixes miss it:** `mw-compose-of`'s reclamation is caller-side
and keyed on the variadic-rest `EX_CONS_LIST` at the call site (distinct
let-bound factories + a `^borrow` rest param + a spine-free in the callee). Here
the list is built by `build-chain` recursion into an anonymous runtime value; the
factories are not distinct let-bindings, `composed` never reaches a
`let_binding_env_freeable` site, and the callee (`httpd-mw-fold`) does not own the
externally-built `chain`.

**Fix directions:** this is the general "owned runtime list of closures consumed
by a fold" case -- R3 (refcount the env) or a general owned-`list<Closure>` drop
(the holder/fold owns and drops each element + the spine). Partial, cheaper wins
if it is ever prioritized without the general feature: `^fat next` on `mw-count`
(frees the onion once `composed` is dropped) + freeing `composed` at the let's
scope exit + a `httpd-mw-free-spine`-style spine free over `chain`. See
`docs/upcoming/closure-drop-glue-plan.md` R2/R3.
