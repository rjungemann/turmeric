// primes -- sieve of Eratosthenes; mirrors the Turmeric/C sieve exactly.
use std::env;

fn count_primes(limit: i64) -> i64 {
    let limit = limit as usize;
    let mut sieve = vec![0u8; limit + 1];
    let mut count: i64 = 0;
    let mut i = 2usize;
    while i <= limit {
        if sieve[i] == 0 {
            let mut j = 2 * i;
            while j <= limit {
                sieve[j] = 1;
                j += i;
            }
            count += 1;
        }
        i += 1;
    }
    count
}

fn main() {
    let n: i64 = env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(10000);
    println!("{}", count_primes(n));
}
