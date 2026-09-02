// sort -- fill n values via LCG (logical >>1 to stay non-negative), sort
// ascending, print min and max.
use std::env;

const LCG_A: u64 = 6364136223846793005;
const LCG_C: u64 = 1442695040888963407;

fn main() {
    let n: i64 = env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(1000);
    let n = n as usize;
    let mut arr: Vec<u64> = Vec::with_capacity(n);
    let mut state: u64 = 12345;
    for _ in 0..n {
        state = state.wrapping_mul(LCG_A).wrapping_add(LCG_C);
        arr.push(state >> 1);
    }
    arr.sort_unstable();
    println!("{} {}", arr[0], arr[n - 1]);
}
