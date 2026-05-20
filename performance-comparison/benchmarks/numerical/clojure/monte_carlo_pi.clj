(let [iters (Long/parseLong (first *command-line-args*))
      inside (loop [i 0 state (long 6364136223846793005) inside 0]
               (if (>= i (* 2 iters))
                 inside
                 (let [s1 (unchecked-add (unchecked-multiply state 6364136223846793005)
                                         1442695040888963407)
                       x  (/ (double (unsigned-bit-shift-right s1 11))
                              (double (bit-shift-left 1 53)))
                       s2 (unchecked-add (unchecked-multiply s1 6364136223846793005)
                                         1442695040888963407)
                       y  (/ (double (unsigned-bit-shift-right s2 11))
                              (double (bit-shift-left 1 53)))]
                   (recur (+ i 2) s2 (if (<= (+ (* x x) (* y y)) 1.0)
                                       (inc inside) inside)))))]
  (printf "%.6f%n" (* 4.0 inside (/ 1.0 iters))))
