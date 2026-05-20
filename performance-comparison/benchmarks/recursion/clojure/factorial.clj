(defn factorial [n acc mod-val]
  (if (<= n 1) (mod acc mod-val)
      (recur (dec n) (mod (* acc n) mod-val) mod-val)))

(let [n (Long/parseLong (first *command-line-args*))]
  (println (factorial n 1 1000000007)))
