(let [n (Long/parseLong (first *command-line-args*))
     result (loop [i 0 a 1.0 b 1.0]
               (if (>= i n) (+ a b)
                   (recur (inc i)
                          (+ (* a 1.0000001) (Math/sqrt b))
                          (+ (* b 0.9999999) (Math/sqrt a)))))]
  (printf "%.6f%n" result))
