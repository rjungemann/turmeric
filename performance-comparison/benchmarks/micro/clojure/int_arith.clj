(let [n (Long/parseLong (first *command-line-args*))
     result (loop [i 0 a (long 1) b (long 1)]
               (if (>= i n) (bit-xor a b)
                   (recur (inc i)
                          (unchecked-add (unchecked-multiply a 1000003) b)
                          (unchecked-add (unchecked-multiply b 999983) a))))]
  (println result))
