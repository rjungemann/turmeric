(let [n   (Long/parseLong (first *command-line-args*))
      arr (loop [i 0 s (long 12345) acc (transient [])]
            (if (>= i n) (persistent! acc)
                (let [ns (unchecked-add (unchecked-multiply s 6364136223846793005)
                                        1442695040888963407)]
                  (recur (inc i) ns (conj! acc (unsigned-bit-shift-right ns 1))))))
      sorted (sort arr)]
  (println (first sorted) (last sorted)))
