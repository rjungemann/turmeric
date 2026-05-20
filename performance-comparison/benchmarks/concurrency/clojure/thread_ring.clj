(let [n-threads  (Integer/parseInt (first *command-line-args*))
     messages    (Long/parseLong (second *command-line-args*))
     ;; adjust so token reaches 0 at thread 0
     adj         (mod (- n-threads (mod messages n-threads)) n-threads)
     total       (+ messages adj)
     chans       (vec (repeatedly n-threads #(java.util.concurrent.LinkedBlockingQueue. 1)))
     latch       (java.util.concurrent.CountDownLatch. 1)]
  (doseq [i (range n-threads)]
    (let [me   (nth chans i)
          next (nth chans (mod (inc i) n-threads))]
      (future
        (loop []
          (let [tok (.take me)]
            (cond
              (<= tok 0)
              (when (= i 0) (.countDown latch))
              :else
              (do (.put next (dec tok)) (recur))))))))
  (.put (first chans) total)
  (.await latch)
  (println "done"))
