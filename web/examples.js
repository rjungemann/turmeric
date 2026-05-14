/**
 * Try Turmeric - Code Examples
 * This file contains all the example code snippets for the web REPL
 */

// Example code snippets organized by category
export const EXAMPLES = {
    // Hello World
    hello: {
        title: 'Hello World',
        code: `(println "Hello, Turmeric!")`,
        description: 'The simplest Turmeric program'
    },

    // Basic Math
    math: {
        title: 'Basic Math',
        code: `;; Basic arithmetic operations
(+ 1 2 3)
(- 10 5)
(* 2 3 4)
(/ 100 4)

;; Modulo
(% 10 3)

;; Power (using exponentiation)
;; Note: Turmeric uses ** for exponentiation
(** 2 8)`,
        description: 'Basic arithmetic with integers'
    },

    // Factorial
    factorial: {
        title: 'Factorial',
        code: `;; Recursive factorial function with type annotations
(defn factorial [n :int] :int
  (if (<= n 1) 1 (* n (factorial (- n 1)))))

;; Compute factorial of 10
(factorial 10)

;; Compute factorial of 5
(factorial 5)`,
        description: 'Recursive function with type annotations'
    },

    // Fibonacci
    fibonacci: {
        title: 'Fibonacci Sequence',
        code: `;; Recursive Fibonacci function
(defn fib [n :int] :int
  (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2)))))

;; Compute Fibonacci numbers
(fib 0)
(fib 1)
(fib 10)
(fib 15)`,
        description: 'Recursive Fibonacci sequence'
    },

    // Closures
    closure: {
        title: 'Closures',
        code: `;; Function that returns a closure
(defn make-adder [x :int] (fn [y :int] (+ x y)))

;; Create closures
(let [add5 (make-adder 5)]
  (println (add5 10))  ;; Prints 15
  (println (add5 20))) ;; Prints 25

(let [add10 (make-adder 10)]
  (println (add10 5))   ;; Prints 15
  (println (add10 32))) ;; Prints 42

;; Multiple closures sharing state
(defn make-counter []
  (let [count (rc/of 0)]
    (fn []
      (let [current (rc/clone count)]
        (rc/drop count)
        (let [new-count (rc/clone current)]
          (println @new-count)
          (rc/drop current))))))

(let [counter (make-counter)]
  (counter)  ;; Prints 0
  (counter)  ;; Prints 0
  (counter))  ;; Prints 0`,
        description: 'Closures and lexical scoping'
    },

    // Algebraic Effects
    effects: {
        title: 'Algebraic Effects',
        code: `;; Define a custom effect
(defeffect Ask [] :int)

(defn use-ask [] :int
  (+ 1 (perform (Ask))))

;; Handle the effect
(println (handle (use-ask)
  (Ask [] k) (resume k 41)))

;; More complex effect example with multiple cases
(defeffect Logger [msg :str] :nil)

(defn log-and-add [x :int y :int] :int
  (perform (Logger (str (+ "Adding " x " and " y))))
  (+ x y))

(println (handle (log-and-add 3 4)
  (Logger [msg k] (println msg) (resume k nil))))`,
        description: 'Algebraic effects and handlers'
    },

    // Reference Counting
    rc: {
        title: 'Reference Counting',
        code: `;; Reference counting example
(defn main [] :int
  ;; Create a reference-counted value
  (let [r (rc/of 42)]
    (println (str "Initial strong count: " (rc/strong-count r)))
    
    ;; Clone the reference (increases strong count)
    (let [r2 (rc/clone r)]
      (println (str "After clone, strong count: " (rc/strong-count r)))
      (println (str "r and r2 are equal: " (= @r @r2)))
      
      ;; Drop the clone
      (rc/drop r2))
    
    ;; Strong count is back to 1
    (println (str "After dropping r2, strong count: " (rc/strong-count r)))
    
    ;; Drop the original
    (rc/drop r)
    0))`,
        description: 'Reference counting with rc/of, rc/clone, and rc/drop'
    },

    // Type Annotations
    types: {
        title: 'Type Annotations',
        code: `;; Function with type annotations
(defn add [a :int b :int] :int
  (+ a b))

(add 1 2)

;; Function with polymorphic type
(defn identity [x :a] :a x)

(identity 42)
(identity "hello")

;; Struct with type annotations
(defstruct Point [x :int y :int])

(let [p (Point 10 20)]
  (println (str "Point: (" (.x p) ", " (.y p) ")")))`,
        description: 'Type annotations and structs'
    },

    // Pattern Matching
    matching: {
        title: 'Pattern Matching',
        code: `;; Pattern matching with defn
(defn factorial [n :int] :int
  (match n
    0 1
    1 1
    _ (* n (factorial (- n 1)))))

(factorial 5)

;; Pattern matching with structs
defstruct Point [x :int y :int]

defn describe-point [p :Point] :str
  (match p
    (Point 0 0) "Origin"
    (Point x y) (str "Point at (" x ", " y ")")))

(describe-point (Point 0 0))
(describe-point (Point 3 4))`,
        description: 'Pattern matching with match expressions'
    },

    // Vectors
    vectors: {
        title: 'Vectors',
        code: `;; Vector operations
(let [v1 [1 2 3]]
  (println (str "Vector: " v1))
  (println (str "First element: " (get v1 0)))
  (println (str "Length: " (len v1))))

;; Vector concatenation
(let [v2 [4 5 6]]
  (println (str "Concatenated: " (concat v1 v2))))

;; Vector mapping
(let [doubled (map (fn [x] (* x 2)) [1 2 3 4])]
  (println (str "Doubled: " doubled)))

;; Vector filtering
(let [evens (filter (fn [x] (= (% x 2) 0)) [1 2 3 4 5 6])]
  (println (str "Evens: " evens)))`,
        description: 'Vector operations and functional programming'
    },

    // Higher-Order Functions
    higher_order: {
        title: 'Higher-Order Functions',
        code: `;; Higher-order function example
(defn twice [f :(fn [a] :a) x :a] :a
  (f (f x)))

;; Define a function to increment
(defn inc [x :int] :int (+ x 1))

;; Apply it twice
(twice inc 5)  ;; Returns 7

;; Compose functions
(defn compose [f :(fn [a] :b) g :(fn [c] :a)] :(fn [c] :b)
  (fn [x] (f (g x))))

(let [add1 (fn [x] (+ x 1))]
  (let [add2 (compose add1 add1)]
    (add2 5)))  ;; Returns 7

;; Map, filter, reduce
(let [numbers [1 2 3 4 5]]
  (println (str "Doubled: " (map (fn [x] (* x 2)) numbers)))
  (println (str "Evens: " (filter (fn [x] (= (% x 2) 0)) numbers)))
  (println (str "Sum: " (reduce (fn [a b] (+ a b)) 0 numbers))))`,
        description: 'Higher-order functions and functional composition'
    }
};

// Categorized examples for the dropdown
export const EXAMPLE_CATEGORIES = {
    basics: [
        { id: 'hello', name: 'Hello World' },
        { id: 'math', name: 'Basic Math' },
        { id: 'types', name: 'Type Annotations' }
    ],
    functions: [
        { id: 'factorial', name: 'Factorial' },
        { id: 'fibonacci', name: 'Fibonacci' },
        { id: 'closure', name: 'Closures' },
        { id: 'higher_order', name: 'Higher-Order Functions' }
    ],
    advanced: [
        { id: 'effects', name: 'Algebraic Effects' },
        { id: 'rc', name: 'Reference Counting' },
        { id: 'matching', name: 'Pattern Matching' },
        { id: 'vectors', name: 'Vectors' }
    ]
};

// Get example by ID
export function getExample(id) {
    return EXAMPLES[id];
}

// Get all example IDs
export function getAllExampleIds() {
    return Object.keys(EXAMPLES);
}

// Format example for display
export function formatExample(id) {
    const example = getExample(id);
    if (!example) return null;
    
    return {
        id,
        title: example.title || id,
        code: example.code,
        description: example.description || ''
    };
}
