# A `^deprecated` wrapper declared above its class's first instance silently stops warning

**Severity: medium** -- not a wrong answer, but a **silently missing
diagnostic**, which for a deprecation shim is the whole product. The program
compiles and runs correctly; it just never tells anyone to migrate. The
failure is invisible at the definition site and invisible at every call site.

**Status:** open. Found 2026-09-13 while prototyping the class-alias half of
the [msgpack spice plan](../upcoming/hold/msgpack-spice-plan.md)'s
`Encode` -> `EncodeJson` rename, where a `^deprecated` forwarding wrapper is
the entire migration mechanism.

## Repro

```turmeric
(defclass EncodeJson [a] (encode-json [x] : cstr))

;; The shim, declared BEFORE any instance of EncodeJson.
(defn ^deprecated "use `encode-json`" encode [^EncodeJson A] [x : A] : cstr
  (encode-json x))

(definstance EncodeJson [int] (encode-json [x] : cstr
  ```c
  char *b=(char*)malloc(24); snprintf(b,24,"%lld",(long long)x); return b;
  ```))

(defn main [] : int (println (encode 42)) 0)
```

```
$ tur check p.tur | grep -c "is deprecated"
0                     # <-- expected 1, at the (encode 42) call
$ tur run p.tur
42                    # correct answer, no warning
```

Move the `defn` below the `definstance` and nothing else, and the warning
appears:

```
p.tur:13:22: warning: 'encode' is deprecated: use `encode-json`
```

Verified on a stock v0.47.0 Debug build.

## Why it matters

A deprecation shim exists only to emit the warning; a silent one is worse
than no shim, because the author believes consumers are being told to
migrate and they are not. The removal release then breaks them with no
prior signal. `--Werror=deprecated`, the mechanical way a consumer finds
every site, also finds nothing.

The ordering that triggers it is the natural one to write. Declaring the
public wrapper above the instance table reads better and is what a `defn`
normally allows -- nothing else in the language makes a top-level `defn`
order-sensitive against a `definstance`.

## Root cause (partial)

Adjacent and probably the same mechanism: `json/encode.tur:166` in
turmeric-spices already carries a hand-written note on `encode-string`
saying "a polymorphic wrapper over a typeclass method must follow at least
one instance of that class, or method resolution fails with 'no typeclass
method found'". So the ordering constraint is known. What is NOT known --
and is this report -- is that when the wrapper's body resolves anyway, the
`^deprecated` attribute on its *binding* stops being consulted at use sites.

The use-site warning fires from the binding-resolution path
(`src/compiler/elab_module.c:1666`, guarded on `b->is_deprecated`). The call
`(encode 42)` in the broken ordering evidently does not reach that path --
most likely it is being routed through the typeclass method-dispatch path
(`elab_call.c`'s `elab_user_method_instance_matches` / `elab_method_call`)
rather than resolved as an ordinary binding, so no `Binding` with
`is_deprecated` is ever consulted. Not confirmed; the bisect above
establishes the trigger and the symptom, not the exact branch.

## Fix directions

1. Make the ordering irrelevant: whatever path handles the call in the
   broken ordering should consult the same `is_deprecated` flag. If that
   path resolves to a `FnDef`/`Binding` at all, the check is a copy of
   `elab_module.c:1666`.
2. Failing that, make it loud instead of silent: warn at the `defn` when a
   `^deprecated` wrapper is declared above the first instance of a class it
   is constrained on, telling the author to move it down. Strictly worse
   than (1) but it removes the silence.

A fixture pinning the working ordering already exists
(`tests/fixtures/deprecated-shim-over-return-dispatch-method`); it declares
the shim below the instances and so does not cover this. A companion
negative fixture should pin the warning count for the above-ordering once
the behavior is decided.

## Workaround

Declare every `^deprecated` wrapper **below** at least one `definstance` of
the class it constrains, and assert the warning count in a test rather than
assuming it fires.
