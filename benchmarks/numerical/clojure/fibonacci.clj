(ns fibonacci
  (:gen-class)
  (:import [java.lang System]))

(defn recursive-fib [n]
  (if (<= n 1)
    n
    (+ (recursive-fib (- n 1))
       (recursive-fib (- n 2)))))

(defn iterative-fib [n]
  (if (<= n 1)
    n
    (loop [a 0 b 1 i 2]
      (if (> i n)
        b
        (recur b (+ a b) (inc i))))))

(defn -main [& args]
  (let [n (Integer/parseInt (first args))
        start (System/currentTimeMillis)
        result (recursive-fib n)
        time (- (System/currentTimeMillis) start)]
    (println (format "{"language": "Clojure", "version": "%s", "implementation": "recursive", "n": %d, "result": %d, "time_ms": %d}"
                     (clojure-version) n result time))

    (let [start (System/currentTimeMillis)
          result (iterative-fib n)
          time (- (System/currentTimeMillis) start)]
      (println (format "{"language": "Clojure", "version": "%s", "implementation": "iterative", "n": %d, "result": %d, "time_ms": %d}"
                       (clojure-version) n result time)))))