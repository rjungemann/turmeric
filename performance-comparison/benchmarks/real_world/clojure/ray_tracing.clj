(let [width  (Integer/parseInt (first *command-line-args*))
     height  (Integer/parseInt (second *command-line-args*))
     spheres [{:cx 0 :cy 0 :cz -5 :r 1.0}
               {:cx 2 :cy 0 :cz -7 :r 1.5}
               {:cx -3 :cy 0 :cz -6 :r 0.8}]
     dot     (fn [ax ay az bx by bz] (+ (* ax bx) (* ay by) (* az bz)))
     norm    (fn [x y z] (let [l (+ (Math/sqrt (dot x y z x y z)) 1e-15)]
                           [(/ x l) (/ y l) (/ z l)]))
     [lx ly lz] (norm 1 1 -1)
     hit     (fn [cx cy cz r ox oy oz dx dy dz]
               (let [ocx (- ox cx) ocy (- oy cy) ocz (- oz cz)
                     a (dot dx dy dz dx dy dz)
                     b2 (dot ocx ocy ocz dx dy dz)
                     c (- (dot ocx ocy ocz ocx ocy ocz) (* r r))
                     disc (- (* b2 b2) (* a c))]
                 (when (>= disc 0)
                   (let [t (/ (- (- b2) (Math/sqrt disc)) a)]
                     (when (> t 0.001) t)))))
     checksum (atom 0)]
  (dotimes [y height]
    (dotimes [x width]
      (let [u (- (* (/ (double x) width) 2) 1)
            v (- (* (/ (double y) height) 2) 1)
            [dx dy dz] (norm u v -1)
            result (reduce (fn [best s]
                             (let [t (hit (:cx s) (:cy s) (:cz s) (:r s) 0 0 0 dx dy dz)]
                               (if (and t (< t (:t best)))
                                 {:t t :s s} best)))
                           {:t 1e18 :s nil} spheres)]
        (when (:s result)
          (let [t  (:t result) s (:s result)
                hx (* dx t) hy (* dy t) hz (* dz t)
                [nx ny nz] (norm (- hx (:cx s)) (- hy (:cy s)) (- hz (:cz s)))
                diff (max 0.0 (dot nx ny nz lx ly lz))]
            (swap! checksum + (long (* diff 255))))))))
  (println @checksum))
