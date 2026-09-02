// int_arith -- wrapping u64 multiply-add ladder (b reads the updated a),
// xor the two accumulators, print as signed i64 bit-cast (matches C's
// %lld over int64_t and the Haskell Word64->Int64 column).
use std::env;

fn int_arith_loop(n: i64) -> i64 {
    let mut a: u64 = 1;
    let mut b: u64 = 1;
    let mut i: i64 = 0;
    while i < n {
        let new_a = a.wrapping_mul(1000003).wrapping_add(b);
        let new_b = b.wrapping_mul(999983).wrapping_add(new_a);
        a = new_a;
        b = new_b;
        i += 1;
    }
    (a ^ b) as i64
}

fn main() {
    let n: i64 = env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(1000000);
    println!("{}", int_arith_loop(n));
}
