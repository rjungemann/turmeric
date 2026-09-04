// float_arith -- FP ladder, both updates computed from the OLD a and b;
// %.6f output (Rust's fixed-precision float formatting is an exact
// decimal expansion, matching C's printf -- no shortest-repr divergence).
use std::env;

fn float_arith_loop(n: i64) -> f64 {
    let mut a: f64 = 1.0;
    let mut b: f64 = 1.0;
    let mut i: i64 = 0;
    while i < n {
        let new_a = a * 1.0000001 + b.sqrt();
        let new_b = b * 0.9999999 + a.sqrt();
        a = new_a;
        b = new_b;
        i += 1;
    }
    a + b
}

fn main() {
    let n: i64 = env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(1000000);
    println!("{:.6}", float_arith_loop(n));
}
