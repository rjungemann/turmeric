// fibonacci -- iterative Fibonacci; i64 wraps like the C/Turmeric column.
use std::env;

fn fib(n: i64) -> i64 {
    if n <= 1 {
        return n;
    }
    let mut a: i64 = 0;
    let mut b: i64 = 1;
    let mut i: i64 = 2;
    while i <= n {
        let next = a.wrapping_add(b);
        a = b;
        b = next;
        i += 1;
    }
    b
}

fn main() {
    let n: i64 = env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(1000);
    println!("{}", fib(n));
}
