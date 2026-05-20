(let [file-size (Long/parseLong (first *command-line-args*))
     n-reads    (Long/parseLong (second *command-line-args*))
     path       "/tmp/bench_io_random_clj.bin"]
  ;; write deterministic data
  (with-open [fw (java.io.RandomAccessFile. path "rw")]
    (.setLength fw file-size)
    (loop [pos 0 seq-v 0]
      (when (< pos file-size)
        (let [chunk (int (min 4096 (- file-size pos)))
              buf   (byte-array chunk (map unchecked-byte (range seq-v (+ seq-v chunk))))]
          (.seek fw pos)
          (.write fw buf)
          (recur (+ pos chunk) (+ seq-v chunk))))))
  ;; random reads
  (let [raf (java.io.RandomAccessFile. path "r")
        checksum (loop [i 0 state (long 12345678) cs (long 0)]
                   (if (>= i n-reads) cs
                     (let [state2 (unchecked-add (unchecked-multiply state 6364136223846793005) 1442695040888963407)
                           offset (Math/abs (rem (unsigned-bit-shift-right state2 1) file-size))]
                       (.seek raf offset)
                       (recur (inc i) state2 (+ cs (.read raf))))))]
    (.close raf)
    (.delete (java.io.File. path))
    (println checksum)))
