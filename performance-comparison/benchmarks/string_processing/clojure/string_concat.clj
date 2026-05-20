(let [n   (Long/parseLong (first *command-line-args*))
      s   (apply str (repeat n "hello"))]
  (println (count s)))
