// tutorials.js — Tutorial step data for the Turmeric web REPL
// Each tutorial is an array of steps with title, description, starter code, and answer.

export const TUTORIAL_STEPS = {
    'basics': [
        {
            title: 'Step 1: Hello World',
            description: 'Print "Hello, Turmeric!" to the console using println.',
            starter: ';; Print "Hello, Turmeric!" to the console\n',
            answer: '(println "Hello, Turmeric!")'
        },
        {
            title: 'Step 2: Arithmetic',
            description: 'Add 1, 2, and 3 together using the + operator.',
            starter: ';; Add 1, 2, and 3 together\n',
            answer: '(+ 1 2 3)'
        },
        {
            title: 'Step 3: Variables',
            description: 'Bind the value 42 to the name x using let, then print it.',
            starter: ';; Bind 42 to x and print it\n',
            answer: '(let [x 42]\n  (println x))'
        },
        {
            title: 'Step 4: Conditionals',
            description: 'Use if to print "big" when 10 > 5, otherwise "small".',
            starter: ';; Print "big" if 10 > 5, otherwise "small"\n',
            answer: '(println (if (> 10 5) "big" "small"))'
        }
    ],
    'data-structures': [
        {
            title: 'Step 1: Vectors',
            description: 'Create a vector [1 2 3] and print its first element using get.',
            starter: ';; Create a vector and access its first element\n',
            answer: '(let [v [1 2 3]]\n  (println (get v 0)))'
        },
        {
            title: 'Step 2: Structs',
            description: 'Define a Point struct with x and y fields, create a Point at (3, 4), and print its fields.',
            starter: ';; Define a Point struct and create an instance at (3, 4)\n',
            answer: '(defstruct Point [x :int y :int])\n\n(let [p (Point 3 4)]\n  (println (.x p))\n  (println (.y p)))'
        },
        {
            title: 'Step 3: Pattern Matching',
            description: 'Use match to print "zero", "one", or "other" for the value of n.',
            starter: ';; Match n and print "zero", "one", or "other"\n(let [n 1]\n  )',
            answer: '(let [n 1]\n  (println (match n\n    0 "zero"\n    1 "one"\n    _ "other")))'
        },
        {
            title: 'Step 4: Map and Filter',
            description: 'Double each number in [1 2 3 4], then keep only those greater than 4.',
            starter: ';; Double [1 2 3 4], then filter to keep elements > 4\n',
            answer: '(let [doubled (map (fn [x] (* x 2)) [1 2 3 4])]\n  (println (filter (fn [x] (> x 4)) doubled)))'
        }
    ],
    'type-system': [
        {
            title: 'Step 1: Type Annotations',
            description: 'Define an "add" function with :int annotations on both parameters and the return type.',
            starter: ';; Define add with type annotations, then call it with 3 and 4\n',
            answer: '(defn add [a :int b :int] :int\n  (+ a b))\n\n(println (add 3 4))'
        },
        {
            title: 'Step 2: Polymorphic Functions',
            description: 'Write an "identity" function using a type variable :a that works on any type.',
            starter: ';; Define a polymorphic identity function and test it on an int and a string\n',
            answer: '(defn identity [x :a] :a x)\n\n(println (identity 42))\n(println (identity "hello"))'
        },
        {
            title: 'Step 3: Struct Types',
            description: 'Define a Point struct and a "get-x" function that takes a :Point and returns its x field.',
            starter: ';; Define Point and a function that extracts its x field\n',
            answer: '(defstruct Point [x :int y :int])\n\n(defn get-x [p :Point] :int (.x p))\n\n(println (get-x (Point 10 20)))'
        }
    ],
    'functions': [
        {
            title: 'Step 1: Defining Functions',
            description: 'Define a "square" function that multiplies its argument by itself.',
            starter: ';; Define square and call it with 7\n',
            answer: '(defn square [n :int] :int (* n n))\n\n(println (square 7))'
        },
        {
            title: 'Step 2: Recursion',
            description: 'Write a recursive "factorial" function.',
            starter: ';; Define a recursive factorial function and compute (factorial 5)\n',
            answer: '(defn factorial [n :int] :int\n  (if (<= n 1) 1 (* n (factorial (- n 1)))))\n\n(println (factorial 5))'
        },
        {
            title: 'Step 3: Closures',
            description: 'Write a "make-adder" function that returns a closure adding x to its argument.',
            starter: ';; Define make-adder, create add5, and call it with 10\n',
            answer: '(defn make-adder [x :int] (fn [y :int] (+ x y)))\n\n(let [add5 (make-adder 5)]\n  (println (add5 10)))'
        },
        {
            title: 'Step 4: Higher-Order Functions',
            description: 'Use map to double every element in [1 2 3 4 5].',
            starter: ';; Use map to double every element in [1 2 3 4 5]\n',
            answer: '(println (map (fn [x] (* x 2)) [1 2 3 4 5]))'
        }
    ],
    'effects': [
        {
            title: 'Step 1: Defining Effects',
            description: 'Declare an Ask effect that yields an int, perform it, and handle it by resuming with 41.',
            starter: ';; Define Ask, perform it in a function, and handle with resume k 41\n',
            answer: '(defeffect Ask [] :int)\n\n(defn use-ask [] :int\n  (+ 1 (perform (Ask))))\n\n(println (handle (use-ask)\n  (Ask [] k) (resume k 41)))'
        },
        {
            title: 'Step 2: Effects with Logging',
            description: 'Define a Logger effect that prints a message, then returns a value from the computation.',
            starter: ';; Define Logger, log "hello", return 42, and handle by printing the message\n',
            answer: '(defeffect Logger [msg :str] :nil)\n\n(defn logged-value [] :int\n  (perform (Logger "hello"))\n  42)\n\n(println (handle (logged-value)\n  (Logger [msg k] (println msg) (resume k nil))))'
        },
        {
            title: 'Step 3: Multiple Effects',
            description: 'Handle two different effects -- Ask and Log -- in a single handle block.',
            starter: ';; Define Ask and Log effects, use both in a program, handle both together\n',
            answer: '(defeffect Ask [] :int)\n(defeffect Log [msg :str] :nil)\n\n(defn program [] :int\n  (perform (Log "starting"))\n  (+ 1 (perform (Ask))))\n\n(println (handle (program)\n  (Ask [] k) (resume k 10)\n  (Log [msg k] (println msg) (resume k nil))))'
        }
    ]
};
