// function_call -- n calls through a non-inlined function, measuring call
// overhead rather than a fused loop (mirrors Haskell's NOINLINE inc1).
use std::env;
use std::hint::black_box;

#[inline(never)]
fn inc1(x: i64) -> i64 {
    black_box(x) + 1
}

fn main() {
    let n: i64 = env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(1000000);
    let mut v: i64 = 0;
    for _ in 0..n {
        v = inc1(v);
    }
    println!("{}", v);
}
