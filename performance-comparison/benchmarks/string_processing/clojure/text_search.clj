(import '[clojure.string :as str])
(let [hs-size (Long/parseLong (first *command-line-args*))
      hay     (apply str (map #(if (< (mod % 10) 5) "x" (str (get "hello" (- (mod % 10) 5))))
                              (range hs-size)))
      needle  "hello"
      nlen    (count needle)
      cnt     (loop [pos 0 c 0]
                (let [i (.indexOf hay needle pos)]
                  (if (< i 0) c
                      (recur (+ i nlen) (inc c)))))]
  (println cnt))
