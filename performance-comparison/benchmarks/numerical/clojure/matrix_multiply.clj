(defn make-matrix [n init]
  (vec (repeatedly n #(vec (repeat n init)))))

(defn matmul [A B n]
  (vec
    (for [i (range n)]
      (vec
        (for [j (range n)]
          (reduce + (map #(* (get-in A [i %]) (get-in B [% j]))
                         (range n))))))))

(let [n (Integer/parseInt (first *command-line-args*))
      A (make-matrix n 1.0)
      B (make-matrix n 1.0)
      C (matmul A B n)
      s (reduce + (map #(reduce + %) C))]
  (println (long s)))
