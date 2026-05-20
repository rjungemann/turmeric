(let [n (Long/parseLong (first *command-line-args*))
     result (loop [i 0 a (long 1) b (long 1)]
               (if (>= i n) (bit-xor a b)
                   (let [new-a (unchecked-add (unchecked-multiply a 1000003) b)]
                     (recur (inc i)
                            new-a
                            (unchecked-add (unchecked-multiply b 999983) new-a)))))]
  (println result))
