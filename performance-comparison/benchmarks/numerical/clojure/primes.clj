(defn count-primes [limit]
  (let [sieve (boolean-array (inc limit))]
    (loop [i 2 count 0]
      (cond
        (> i limit) count
        (aget sieve i) (recur (inc i) count)
        :else (do
                (loop [j (* 2 i)]
                  (when (<= j limit)
                    (aset sieve j true)
                    (recur (+ j i))))
                (recur (inc i) (inc count)))))))

(let [n (Long/parseLong (first *command-line-args*))]
  (println (count-primes n)))
