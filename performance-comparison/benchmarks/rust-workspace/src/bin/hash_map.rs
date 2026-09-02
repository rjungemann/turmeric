// hash_map -- insert i -> i*2 for i in [0, n), then sum every value back
// out by key lookup. std::collections::HashMap is Rust's analogous map to
// the Turmeric HAMT / Haskell IntMap columns (docs/methodology.md).
use std::collections::HashMap;
use std::env;

fn hashmap_bench(n: i64) -> i64 {
    let mut m: HashMap<i64, i64> = HashMap::new();
    for i in 0..n {
        m.insert(i, i * 2);
    }
    let mut sum: i64 = 0;
    for i in 0..n {
        sum += *m.get(&i).unwrap_or(&0);
    }
    sum
}

fn main() {
    let n: i64 = env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(1000);
    println!("{}", hashmap_bench(n));
}
