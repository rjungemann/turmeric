# Wide `:copy`-struct functor + Show dispatch: emitted dict symbol undeclared

**Summary:** A van-Laarhoven lens whose functor is a WIDE `:copy` struct (not the
one-int64 `defopaque` carrier) and whose mapper dispatches a second constraint
(`Show`) emits C that references an undeclared dict symbol
(`_un_undict_unNNNN_MMMM`), so `cc` fails. The `defopaque [a] :int` Identity form
of the same program compiles and runs correctly. **Severity: low** (narrow
shape; a hard compile error at `cc`, not a miscompile or silent wrong answer).

## Repro

```turmeric
(defstruct Identity :copy [a] (wrapped : a))          ; WIDE :copy struct functor
(defn mk-id  [A] [x : A]            : (Identity A) (make-struct Identity :wrapped x))
(defn run-id [A] [i : (Identity A)] : A            (.wrapped i))
(definstance Functor [Identity]
  (fmap [i g] (mk-id (g (run-id i)))))

(defclass Show [a] (show [x] : cstr))
(definstance Show [int] (show [x] : cstr (if (< x 0) "neg" "nonneg")))

(defn deep-lens [^f a] [^Functor f ^Show a g : (-> a (f a)) s : a] : (f cstr)
  (fmap (g s) (fn [x : a] : cstr (show x))))          ; depth-0 Show dispatch

(defn use-deep
    [l (forall [(f :: * -> *) a] [(Functor f) (Show a)] (-> (-> a (f a)) a (f cstr)))
     s : int] : cstr
  (run-id (l (fn [x : int] : (Identity int) (mk-id x)) s)))

(defn main [] : int (println (use-deep deep-lens 7)) 0)
```

```
tur run <file>
# /tmp/tur-build/....c: error: '_un_undict_unNNNN_MMMM' undeclared (first use in this function)
# tur: cc invocation failed
```

Swapping the functor to `(defopaque Identity [a] :int)` with inline-C
`mk-id`/`run-id` compiles and prints `nonneg`. The `--interpret` path runs the
`:copy`-struct form correctly (it never hits this codegen path).

## Notes

- Independent of the directly-applied-nested-lambda dispatch shape
  (`forall-dict-direct-applied-nested-lambda-dispatch`, now resolved): this
  fails even in the depth-0 workaround form above, where the mapper dispatches
  `Show` directly with no inner lambda.
- The undeclared symbol is a dict binding the clone's mapper env expects but the
  emitter never declares/defines for a wide `:copy` functor carrying a second
  constraint dict. Likely in the dict-clone / mapper-env emit for a by-value
  aggregate functor (contrast the int64-carrier `defopaque` path, which names
  the dict correctly).
- Compiled fixtures in `tests/fixtures/van-laarhoven-lens-show-*` all use the
  `defopaque [a] :int` Identity, so none exercises the wide `:copy`-struct
  functor with a Show constraint -- this shape is uncovered.

## Fix directions

Trace where the mapper-env dict symbol (`_un_undict_...`, the mangled dict
binding) is referenced vs declared in the emitter for a `:copy`-struct functor
result, and emit the declaration/definition (or thread the existing dict) on the
by-value-aggregate path as the int64-carrier path already does.
