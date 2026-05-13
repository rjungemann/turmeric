tests/fixtures/type-app//input.tur:9:11: error: type-app: unknown type constructor 'Functor'
 6 |   (fmap [container fn] :int))
 7 | 
 8 | ;; Apply Functor to option - this should create a Functor[option] type
 9 | (type-app Functor option)
   |           ^^^^^^^
10 | 
11 | (defn main [] :int 0)
