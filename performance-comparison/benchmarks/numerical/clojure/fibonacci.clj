(defn fib [n]
  (loop [i 2 a 0 b 1]
    (if (> i n)
      b
      (recur (inc i) b (+ a b)))))

(let [n (Long/parseLong (first *command-line-args*))]
  (println (fib n)))
