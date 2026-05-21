(let [n    (Long/parseLong (first *command-line-args*))
     path  "/tmp/bench_io_read_clj.bin"
     wbuf  (byte-array 4096 (unchecked-byte 0xCD))]
  ;; write phase
  (with-open [fw (java.io.FileOutputStream. path)]
    (loop [rem n]
      (when (> rem 0)
        (let [chunk (min 4096 rem)]
          (.write fw wbuf 0 (int chunk))
          (recur (- rem chunk))))))
  ;; read phase
  (let [rbuf (byte-array 4096)
        total (with-open [fr (java.io.FileInputStream. path)]
                (loop [t 0]
                  (let [nr (.read fr rbuf)]
                    (if (< nr 0) t (recur (+ t nr))))))]
    (.delete (java.io.File. path))
    (println total)))
