// alloc_churn -- allocate and free one small heap object per iteration
// (Box, matching the malloc/free round-trip), summing the stored value.
use std::env;

fn churn(n: i64) -> i64 {
    let mut sum: i64 = 0;
    for i in 0..n {
        let p = Box::new(i);
        sum += *p;
        drop(p);
    }
    sum
}

fn main() {
    let n: i64 = env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(10000);
    println!("{}", churn(n));
}
