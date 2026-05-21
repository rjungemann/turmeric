(let [n-bodies (Integer/parseInt (first *command-line-args*))
     steps     (Integer/parseInt (second *command-line-args*))]
  ;; initialize bodies with deterministic LCG
  (let [lcg  (fn [s] (unchecked-add (unchecked-multiply s 6364136223846793005) 1442695040888963407))
        init (loop [i 0 state (long 42) bodies []]
               (if (>= i n-bodies) bodies
                   (let [s1 (lcg state) s2 (lcg s1) s3 (lcg s2)]
                     (recur (inc i) s3
                            (conj bodies {:x  (/ (double (unsigned-bit-shift-right s1 32)) 1e8)
                                          :y  (/ (double (unsigned-bit-shift-right s2 32)) 1e8)
                                          :z  (/ (double (unsigned-bit-shift-right s3 32)) 1e8)
                                          :vx 0.0 :vy 0.0 :vz 0.0
                                          :mass (+ 1.0 (* (mod i 5) 0.5))})))))
        bodies (atom (vec init))]
    (dotimes [_ steps]
      (let [bs @bodies n n-bodies]
        (dotimes [i n]
          (dotimes [j n]
            (when (< i j)
              (let [bi (nth bs i) bj (nth bs j)
                    dx (- (:x bj) (:x bi)) dy (- (:y bj) (:y bi)) dz (- (:z bj) (:z bi))
                    dist (+ (Math/sqrt (+ (* dx dx) (* dy dy) (* dz dz))) 1e-10)
                    f (/ (* (:mass bi) (:mass bj)) (* dist dist dist))]
                (swap! bodies assoc i (-> (nth @bodies i)
                                          (update :vx + (* f dx))
                                          (update :vy + (* f dy))
                                          (update :vz + (* f dz))))
                (swap! bodies assoc j (-> (nth @bodies j)
                                          (update :vx - (* f dx))
                                          (update :vy - (* f dy))
                                          (update :vz - (* f dz))))))))
        (swap! bodies (fn [bs]
                        (mapv (fn [b] (-> b
                                          (update :x + (:vx b))
                                          (update :y + (:vy b))
                                          (update :z + (:vz b)))) bs)))))
    (let [ke (reduce + (map (fn [b] (* 0.5 (:mass b)
                                        (+ (* (:vx b) (:vx b))
                                           (* (:vy b) (:vy b))
                                           (* (:vz b) (:vz b))))) @bodies))]
      (printf "%.4f%n" ke))))
