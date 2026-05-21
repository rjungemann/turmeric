(let [n   (Long/parseLong (first *command-line-args*))
     sum  (loop [i 0 s 0]
            (if (>= i n) s
                (recur (inc i) (+ s i))))]
  (println sum))
