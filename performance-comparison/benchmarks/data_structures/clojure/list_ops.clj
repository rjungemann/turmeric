(let [n (Long/parseLong (first *command-line-args*))
      lst (loop [i 0 acc '()]
            (if (>= i n) acc
                (recur (inc i) (conj acc i))))
      s   (reduce + lst)]
  (println s))
