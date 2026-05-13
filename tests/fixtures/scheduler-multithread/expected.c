tests/fixtures/scheduler-multithread//input.tur:9:2: error: unknown function or operator 'load'
 6 | ;;   3. Run the scheduler and wait
 7 | ;; Expected output: two different thread IDs (order may vary)
 8 | 
 9 | (load "stdlib/fiber.tur")
   |  ^^^^
10 | (load "stdlib/scheduler_mt.tur")
11 | 
tests/fixtures/scheduler-multithread//input.tur:10:2: error: unknown function or operator 'load'
 7 | ;; Expected output: two different thread IDs (order may vary)
 8 | 
 9 | (load "stdlib/fiber.tur")
10 | (load "stdlib/scheduler_mt.tur")
   |  ^^^^
11 | 
12 | (defn fiber-a [] :nil
tests/fixtures/scheduler-multithread//input.tur:13:13: error: unknown function or operator 'fiber-thread-id'
10 | (load "stdlib/scheduler_mt.tur")
11 | 
12 | (defn fiber-a [] :nil
13 |   (println (fiber-thread-id)))
   |             ^^^^^^^^^^^^^^^
14 | 
15 | (defn fiber-b [] :nil
tests/fixtures/scheduler-multithread//input.tur:16:13: error: unknown function or operator 'fiber-thread-id'
13 |   (println (fiber-thread-id)))
14 | 
15 | (defn fiber-b [] :nil
16 |   (println (fiber-thread-id)))
   |             ^^^^^^^^^^^^^^^
17 | 
18 | (defn main [] :int
tests/fixtures/scheduler-multithread//input.tur:19:16: error: unknown function or operator 'scheduler-mt-new'
16 |   (println (fiber-thread-id)))
17 | 
18 | (defn main [] :int
19 |   (let [sched (scheduler-mt-new 2)]
   |                ^^^^^^^^^^^^^^^^
20 |     (let [fa (fiber-new fiber-a 0)]
21 |       (let [fb (fiber-new fiber-b 0)]
