(let [n   (Long/parseLong (first *command-line-args*))
     path "/tmp/bench_io_write_clj.bin"
     buf  (byte-array 4096 (byte 0xAB))]
  (with-open [f (java.io.FileOutputStream. path)]
    (loop [written 0]
      (if (>= written n)
        (println written)
        (let [chunk (min 4096 (- n written))]
          (.write f buf 0 (int chunk))
          (recur (+ written chunk))))))
  (java.io.File. path) (.delete (java.io.File. path)))
