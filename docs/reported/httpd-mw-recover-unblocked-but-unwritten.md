# mw-recover (panic -> 500 middleware) cannot be written: three compiler defects block it

**Severity: medium** (was: low). Found in the 2026-08-20 docs audit; **rewritten
2026-08-20 after attempting the implementation**.

## What changed about this report

The original filing said the blocker was gone: the old `EX_CATCH_UNWIND`
env-propagation defect is fixed, so `mw-recover` was "just unwritten". That is
half right. The *simple* case works -- a `defn` taking a `^fat` handle and
running it under `catch-unwind` compiles and runs correctly. But the
**middleware shape** does not, and every way of spelling it hits a different
compiler defect. `mw-recover` is blocked, by three new blockers rather than the
old one.

All four repros below are self-contained and reproduce on the tip of
`claude/tractable-report-execution-qihiw5`. None of them mentions httpd: the
defects are in closure/fat-handle codegen, and httpd is only where they were
met.

## The shape being attempted

Every shipped middleware is a factory of this form (`mw-log`,
stdlib/httpd.tur):

```turmeric
(defn mw-log [^fat next : int] : ptr<void>
  (let [_n next]
    (fn [c : ptr<void>] : nil
      ... (httpd-call _n c) ...)))
```

`mw-recover` is that, with the `httpd-call` wrapped in `catch-unwind` so a
panicking handler yields a 500 instead of killing the process.

## A. ICE -- a returned NON-CAPTURING closure inside a `let`

```turmeric
(defn wrap [^fat next : int] : ptr<void>
  (let [_n next]
    (fn [c : ptr<void>] : nil
      (when (= 0 1) 0))))
(defn main [] : int 0)
```

```
tur: internal error (ICE): a representation decision disagrees with repr_of at merge-temp.
  repr-shadow merge-temp result type=(fn [ptr<void>] : nil) want=fat-handle got=carrier-i64
```

Adding **any** use of `_n` in the closure body makes it compile -- which is why
`mw-log` is fine and this was never hit. The trigger is the closure being
non-capturing, so it lowers to a bare function pointer where the `ptr<void>`
return wants a fat handle.

**This is the same root cause as
docs/archive/let-bound-noncapturing-lambda-segfaults-as-fn-arg.md**, at a
different boundary: that one was a bare-pointer closure reaching a `:fn`
*argument*, fixed with a signature-keyed adapter; this is one reaching a
`ptr<void>` *return*. Worth fixing together -- the fix there
(`ensure_bare_fnptr_poly_shim`) may generalize.

**With the guard downgraded, (A) compiles and runs correctly:**

```sh
TUR_REPR_NO_SHADOW_ICE=1 tur run A.tur   # warns, exits 0
```

So for this shape the disagreement is benign and the ICE is the only thing
stopping it. That is *not* a licence to relax the guard, though: the archived
segfault above is precisely a bare pointer reaching a consumer that wanted a
fat handle, and it was silent. The guard is reporting a real inconsistency;
the fix is to make the value actually be a fat handle (or to make `repr_of`
agree that a captureless closure is a bare pointer *and* make every consumer
handle one), not to stop asking.

## B. Use-after-free -- captured `^fat` handle passed to a helper

```turmeric
(defn call-it [^fat h : int c : int] : nil
  (when (not (= h 0)) ((:: h (fn [int] nil)) c)))
(defn run-guarded [^fat h : int c : int] : bool
  (err? (catch-unwind (fn [] : int (do (call-it h c) 0)))))
(defn wrap [^fat next : int] : ptr<void>
  (let [_n next]
    (fn [c : int] : nil
      (when (run-guarded _n c) (println -1)))))
(defn target [x : int] : nil (println x))
(defn main [] : int
  (let [w (wrap target)]
    ((:: w (fn [int] nil)) 7)
    ((:: w (fn [int] nil)) 8))     ; <-- dies here
  0)
```

The **first** call succeeds and the second segfaults: the first call's drop
glue frees the captured env. Under ASan on the real httpd fixture this was
`heap-use-after-free ... in httpd_hycall`, freed by `drop_glue___env_NNNN`.

A single call hides it entirely, which is worth knowing when writing a
regression test for this: a one-request fixture would have passed.

## C. Codegen emits an undeclared variable -- direct parameter capture

Skipping the `(let [_n next] ...)` and capturing the parameter directly:

```turmeric
(defn wrap [^fat next : int] : ptr<void>
  (fn [c : int] : nil
    (let [r (catch-unwind (fn [] : int (do (call-it next c) 0)))]
      (when (err? r) (println -1)))))
```

```
/tmp/tur-build/v_tur.c:4130:27: error: 'next_1334' undeclared (first use in this function)
```

The lifted thunk references the enclosing function's parameter without it
being threaded into the env.

## D. ICE -- `catch-unwind` inline in the returned closure

The most natural spelling, and the first one tried:

```turmeric
(defn wrap [^fat next : int] : ptr<void>
  (let [_n next]
    (fn [c : int] : nil
      (let [r (catch-unwind (fn [] : int (do (call-it _n c) 0)))]
        (when (err? r) (println -1))))))
```

Same ICE as (A). Notable because the closure *does* use `_n` -- but only
inside the nested `catch-unwind` thunk, and that apparently does not register
as the OUTER closure capturing it. So the capture analysis does not see
through a nested lambda.

**Unlike (A), downgrading the guard does not save it** -- a second defect is
waiting underneath:

```sh
TUR_REPR_NO_SHADOW_ICE=1 tur run D.tur
# /tmp/tur-build/..._tur.c:4122:25: error: '__fn_1337' undeclared
```

which is the same class as (C). So (D) needs a real fix, not a guard change,
and is the one that actually gates `mw-recover`.

## Fix direction

(A) and (D) share the ICE, but they are not the same amount of work: (A) is
benign under `TUR_REPR_NO_SHADOW_ICE=1` while (D) hits an undeclared-identifier
codegen bug underneath. So (D) and (C) are the same defect -- a lifted thunk
referencing a name that was never threaded into its env -- and that is the one
that gates `mw-recover`. (B) is a separate lifetime bug in the drop glue for a
captured fat handle. Fixing the (C)/(D) env thread-through plus (B) is enough;
(A) then either falls out or is a guard question on its own.

With any two of these fixed, `mw-recover` is a ten-line middleware:

```turmeric
(defn mw-recover [^fat next : int] : ptr<void>
  (let [_n next]
    (fn [c : ptr<void>] : nil
      (let [r (catch-unwind (fn [] : int (do (httpd-call _n c) 0)))]
        (when (err? r)
          (when (= (httpd-resp-status-get c) 0)
            (do (httpd-resp-status! c 500)
                (httpd-resp-body! c "Internal Server Error"))))))))
```

A fixture for it should send **two** requests -- the panicking one and a
following good one -- since "the server survived" is the property that
matters and (B) shows a single request would not catch a broken one.

## Guides to update when fixed

- docs/guides/httpd-middleware-guide.md ("Not yet shipped" section)
