(let [n   (Long/parseLong (first *command-line-args*))
      m   (java.util.HashMap.)
      _   (dotimes [i n] (.put m i (* i 2)))
      sum (loop [i 0 s 0]
            (if (>= i n) s
                (recur (inc i) (+ s (.get m i)))))]
  (println sum))
