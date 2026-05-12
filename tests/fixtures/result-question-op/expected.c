tests/fixtures/result-question-op/input.tur:48:11: error: ? operator is not yet implemented (Phase R1)
45 |   (if b (ok 42) (err 1)))
46 | 
47 | (defn use-question [b :bool] :ptr<void>
48 |   (let [x (? (get-value b))]
   |           ^^^^^^^^^^^^^^^^^
49 |     (ok (* x 2))))
50 | 
tests/fixtures/result-question-op/input.tur:52:26: error [TUR-E0001]: function 'use-question' arg 1: expected int, got bool
49 |     (ok (* x 2))))
50 | 
51 | (defn main [] :int
52 |   (let [r1 (use-question true)]
   |                          ^^^^
53 |     (let [v1 (ok-val r1)]
54 |       (println v1)
