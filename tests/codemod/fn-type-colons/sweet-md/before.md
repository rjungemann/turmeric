# Toggle pair

A traditional block and its sweet-exp counterpart should both migrate.

```turmeric
(defn run-twice [f : (fn [] #{e} :int)] #{e} : int
  (+ (f) (f)))
```
```sweet-exp
defn run-twice [f :(fn [] #{e} :int)] #{e} :int
  {f() + f()}
```

Prose `:(fn [:int] :int)` outside a fence is left alone.
