# `(defn return ...)` is silently ignored -- call sites resolve to the `return` special form

**Severity:** medium -- the definition is accepted with no error and no warning,
every call site silently binds to the special form instead, and the resulting
diagnostic points at the *caller's* argument type rather than at the shadowing.
`return` is a natural name for a monadic unit, so this is easy to hit and hard
to read.

Verified against `./build/tur` at **v0.32.2** (Debug, tree at `b54ab718e`).

## Repro

These two files are byte-identical except for the function's name.

**Fails** (`return`):

```turmeric
(defn return [x : int] : (fn [] int)
  (fn [] (cons x (tnil))))
(defn call [^fat f : (fn [] int)] : int (f))
(defn main [] : int (call (return 1)))
```
```
z5.tur:4:27: error [TUR-E0001]: function 'call' arg 1: expected (fn [] : int), got int
4 | (defn main [] : int (call (return 1)))
  |                           ^^^^^^^^^^
```

**Passes** (`mkr`):

```turmeric
(defn mkr [x : int] : (fn [] int)
  (fn [] (cons x (tnil))))
(defn call [^fat f : (fn [] int)] : int (f))
(defn main [] : int (call (mkr 1)))
```

Checks clean. The rename is the only difference.

## Root cause

`return` is interned as a special-form symbol at
`src/compiler/elab_core.c:1838`:

```c
e->sym_return    = intern_cstr(st, "return");
```

So `(return 1)` in expression position elaborates as the early-return form,
whose type is the enclosing function's return type (`int` here) -- not as a
call to the user's `defn`. The user's binding is created but never consulted at
the call site.

There is no stdlib `return`; the collision is purely with the special form
(`grep -n "^(defn return " stdlib/*.tur` is empty).

## Why the diagnostic misleads

The error is reported against `call`'s parameter, with the span on
`(return 1)`, and says `expected (fn [] : int), got int`. Nothing mentions
`return`, the special form, or shadowing. Reading it straight, you conclude the
*closure return type* was erased to `int` -- a much scarier and entirely
fictional bug. I chased exactly that for several rounds before bisecting on the
name; a minimal `mk`/`call` repro of the "erasure" theory checks clean, which is
what finally isolated it.

## Fix directions

Roughly in increasing order of cost:

1. **Diagnose the shadowing at the definition.** When a `defn`/`defmacro` name
   matches a reserved special-form symbol, emit a warning (or error) at the
   `defn`. This is the cheap, high-value fix -- it moves the message to the
   line that is actually wrong. A `TUR-W00xx` "definition of 'return' shadows a
   special form and will never be called" would have ended this in one step.
2. **Reserve the name outright.** Make `(defn return ...)` a hard error, the way
   a keyword collision would be in most languages. Stricter, and cheap to
   implement alongside (1), but it is a breaking change for any file that
   currently defines a dead `return`.
3. **Improve the call-site message.** If a call's callee symbol resolves to a
   special form while a same-named user binding exists, say so in the
   diagnostic. Strictly better than today even if (1) lands, since the same
   confusion applies to any other reserved symbol.

Worth auditing the other interned special-form symbols near
`elab_core.c:1838` for the same trap -- `return` is unlikely to be the only
name a user would plausibly reach for.

## Impact seen in-tree

`docs/guides/logic-programming-guide.md` defined its monadic unit as `return`.
Fixed on 2026-07-28 by renaming it to `pure`, which is what made the guide's
`bind`/`mplus` example run for the first time.

## Doc follow-up -- do these when the fix lands

What to change depends on which fix direction is taken:

- **If (1) or (3) -- a warning / better diagnostic, `return` stays reserved:**
  keep `pure` in the logic guide (it is the conventional name for a monadic
  unit anyway, so this is no loss), but **delete the second bullet** ("`return`
  is a reserved special form") from the constraints blockquote under the code
  and drop the link to this report. The trap is then self-diagnosing and does
  not need a guide-level warning.
- **If (2) -- `(defn return ...)` becomes a hard error:** same guide edit, and
  additionally worth a line in whichever doc lists reserved words. If no such
  list exists, that is itself the gap to close -- I could not find one, which
  is part of why this bit.

Independent of which lands: consider a short "reserved names" note in
[docs/guides/syntax-guide.md](../guides/syntax-guide.md). The special-form
symbols interned around `src/compiler/elab_core.c:1838` are not documented as
off-limits anywhere I could find, and `return` is unlikely to be the only one a
user would reach for as a function name.
