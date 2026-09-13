# A generic wrapper that tail-forwards a return-dispatch method fails codegen

**Severity: medium** -- a hard `cc` error, not a wrong answer, and it has a
one-line workaround. But the error surfaces in generated C rather than at the
Turmeric level, so it reads as a compiler bug rather than as "write the body
differently", and the working shape is not discoverable from the message.

**Status:** open. Found 2026-09-13 while checking whether the
[msgpack spice plan](../upcoming/hold/msgpack-spice-plan.md)'s json rename
could keep a compatibility wrapper. Same family as the archived
`return-dispatch-ascription-result-wrapped-not-honored` and
`typeclass-method-parameterized-result-carrier-mismatch`, which are resolved;
this case survives them.

## Repro

```turmeric
(defclass DecodeJson [a] (decode-json [doc : int  val : int] : (Result a cstr)))

(definstance DecodeJson [int] (decode-json [doc val]
  ```c
  (void)doc; return tur_box_ok((int64_t)(val + 100));
  ```))

;; A plain generic wrapper over the method -- the encode-string / decode-list
;; shape from json/encode.tur.
(defn decode [A] [(DecodeJson A)] [doc : int  val : int] : (Result A cstr)
  (decode-json doc val))

(defn main [] : int
  (println (ok-val (:: (decode 0 7) (Result int cstr))))
  0)
```

```
$ tur run p.tur
/tmp/tur-build/p_tur.c: In function
  'decode__spec__tur_adt_Result__int__cstr_int64_t_int64_t':
/tmp/tur-build/p_tur.c:8186:16: error: incompatible types when returning type
  'int64_t' {aka 'long int'} but 'tur_adt_Result__int__cstr' was expected
tur: cc invocation failed (status 256)
```

Elaboration is fine: the call-site ascription pins `A` and re-dispatches
correctly, and a spec is minted per instantiation. Only the emitted return
type disagrees -- the wrapper's spec declares the by-value
`tur_adt_Result__int__cstr` while the forwarded method call yields the int64
carrier.

Ascribing the inner call does **not** help:

```turmeric
  (:: (decode-json doc val) (Result A cstr))   ;; same cc error
```

## Workaround (verified)

Unwrap at `A` and rebuild at the wrapper's own declared result type:

```turmeric
(defn decode [A] [(DecodeJson A)] [doc : int  val : int] : (Result A cstr)
  (let [r (:: (decode-json doc val) (Result A cstr))]
    (if (ok? r) (ok (ok-val r)) (err (err-val r)))))
```

Verified on `int`, `cstr` and a `defstruct` payload, on both `Result` arms,
and through a macro-spliced call site. Pinned by
`tests/fixtures/generic-wrapper-over-return-dispatch-method`.

The rebuild assumes a two-armed `Result`. A class method returning some other
shape would need its own unwrap/rebuild, and one returning an opaque or
non-destructurable type may have no workaround at all -- untested.

## Why it matters

This is the shape of `encode-string` (`json/encode.tur:166`) and
`decode-list` (`:513`) in turmeric-spices: a plain `defn` wrapping a class
method so callers get an ordinary function rather than a bare method call.
`encode-string` does not hit it because `Encode`'s method returns a plain
`cstr`, not a parametric `Result`; any wrapper over a `Decode`-shaped method
does hit it. The msgpack spice plans the same wrapper shape over `DecodeMp`,
so it will meet this the moment it is written.

The cost is not the workaround -- it is that the failure appears as a C type
error in a generated file, several layers below where the author is working.

## Fix directions

The wrapper's monomorphized spec and the forwarded method's return convention
need to agree. Two candidate seams, neither investigated deeply:

1. Teach the spec minting for a constrained-generic wrapper to use the
   carrier convention when its body is a bare tail call to a class method,
   matching what the instance actually returns.
2. Insert the carrier-to-by-value bridge at the tail-call return, which is
   what the manual destructure-and-rebuild is doing by hand.

(2) is closer to how the resolved sibling reports were fixed. Either way the
workaround above stays correct, so this is a papercut to remove rather than a
hole to plug.
