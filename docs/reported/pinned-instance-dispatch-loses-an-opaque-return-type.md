# `@T`-pinned instance dispatch loses the method's return type when it is a `defopaque`

**Severity: low-medium** -- a compile-time rejection with a clear one-token
workaround, so nothing is silently wrong. It is filed because the diagnostic
names a type the user never wrote (`<adt>`), which makes the cause hard to
guess, and because it taxes every call site of an otherwise ordinary API.

**Status:** open. Found 2026-09-14 building `spices/msgpack`, whose `EncodeMp`
method returns an owned `Buf` (a `defopaque`) where the json spice's `Encode`
returns a `cstr`.

## Repro

A class whose method returns a `defopaque`, dispatched with an explicit `@Cons`
pin (needed because a `(Cons A)` shares the `:int` carrier with `int`, so the
call is otherwise ambiguous):

```turmeric
(defopaque Buf :ptr<void>)

(defn __mk-buf [n : int] : ptr<void>
  ```c
  int64_t *p = (int64_t *)malloc(sizeof(int64_t) + 1);
  p[0] = 1;
  ((unsigned char *)(p + 1))[0] = (unsigned char)n;
  return (void *)p;
  ```)

(defn buf-len [b : Buf] : int
  ```c
  return ((int64_t *)(intptr_t)b)[0];
  ```)

(defclass Enc [a] (enc [x] : Buf))

(definstance Enc [int] (enc [x] : Buf (:: (__mk-buf x) Buf)))

(defn frags [A] [(Enc A)] [cell : (Cons A)] : int
  (if (tnil? (:: cell :int))
    (tnil)
    (cons (enc (.head cell)) (frags (:: (.tail cell) (Cons A))))))

(definstance Enc [Cons]
  [(Enc A)]
  (enc [xs] : Buf (:: (__mk-buf 9) Buf)))

(defn main [] : int
  (println (buf-len (enc @Cons (:: (list 1 2) (Cons int)))))
  0)
```

```
$ tur run p.tur
p.tur:34:21: error [TUR-E0001]: function 'buf-len' arg 1: expected Buf, got <adt>
```

`Buf` is what the class, the instance, and both method bodies all declare. The
call site is told it got `<adt>`.

## Controls

| Variant | Result |
| --- | --- |
| Wrap the call: `(:: (enc @Cons ...) Buf)` | **compiles and runs**, prints `1` |
| Same program with the method returning `cstr` instead of `Buf` | **compiles and runs** -- no ascription needed |

The `cstr` control is what `spices/json` does (`encode-json @Cons ...` with no
ascription anywhere), which is why this has not been hit before: json's codec
returns a `cstr` because JSON is text. A binary format cannot -- a msgpack
fragment contains NUL bytes -- so the moment the return type becomes an owned
byte buffer, every pinned call site needs the extra ascription.

## Root cause

Not pinned down in the compiler. The shape of the evidence -- `<adt>` rather
than the carrier type or the opaque's name -- suggests the pinned-dispatch path
resolves the instance but then reports the method's result from the instance's
*head type constructor* (`Cons`) rather than from the method's declared return
type, where the unpinned path carries the declaration through. The relevant
neighbourhood is the pin handling in `src/compiler/elab_call.c`; the two
controls above bracket it (opaque vs. `cstr` return, pinned call only).

## Fix directions

1. Make the pinned path propagate the method's declared return type exactly as
   the unpinned path does. A `defopaque` return should need no more ceremony
   than a `cstr` one.
2. Failing that, the diagnostic should at minimum not say `<adt>` -- naming the
   type it actually inferred would have made this a two-minute fix at the call
   site instead of a bisection.
3. Fixture: a class method returning a `defopaque`, called with an explicit
   `@T` pin, with the `cstr`-returning variant alongside it as the control.

## Workaround in use, and what to remove when this is fixed

`spices/msgpack/tests/container-round-trip.tur` wraps all eight of its pinned
list-encode call sites:

```turmeric
(:: (encode-mp @Cons (:: (list 1 2 3) (Cons int))) Buf)
```

where the natural spelling, and the one the json spice's equivalent test uses,
is just

```turmeric
(encode-mp @Cons (:: (list 1 2 3) (Cons int)))
```

**When this report is resolved, drop the outer `(:: ... Buf)` from those eight
sites** and re-run that suite. Nothing else in the spice is affected -- the
`EncodeMp [Cons]` instance itself is unchanged, and only the `@Cons`-pinned
calls carry the workaround.
