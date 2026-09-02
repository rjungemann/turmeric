// factorial -- tail-recursive-shape factorial mod 1e9+7, expressed as a
// loop (Rust gives no TCO guarantee for the recursive form).
use std::env;

const MODULUS: i64 = 1_000_000_007;

fn fact(n: i64) -> i64 {
    let mut acc: i64 = 1;
    let mut k = n;
    while k > 1 {
        acc = (acc * k) % MODULUS;
        k -= 1;
    }
    acc % MODULUS
}

fn main() {
    let n: i64 = env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(1000);
    println!("{}", fact(n));
}
