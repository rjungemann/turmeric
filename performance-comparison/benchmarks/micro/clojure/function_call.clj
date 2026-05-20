(let [n (Long/parseLong (first *command-line-args*))
     result (loop [i 0 v (long 0)]
               (if (>= i n) v
                   (recur (inc i) (unchecked-inc v))))]
  (println result))
