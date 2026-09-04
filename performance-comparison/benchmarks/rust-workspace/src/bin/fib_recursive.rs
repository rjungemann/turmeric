// fib_recursive -- naive doubly-recursive Fibonacci; genuine recursion
// (not tail-recursive), measuring call overhead like the other columns.
use std::env;

fn fib(n: i64) -> i64 {
    if n <= 1 {
        n
    } else {
        fib(n - 1) + fib(n - 2)
    }
}

fn main() {
    let n: i64 = env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(25);
    println!("{}", fib(n));
}
