// matrix_multiply -- n x n ones, i-k-j order, sum truncated to i64.
use std::env;

fn matmul_checksum(n: i64) -> i64 {
    let n = n as usize;
    let a = vec![1.0f64; n * n];
    let b = vec![1.0f64; n * n];
    let mut c = vec![0.0f64; n * n];
    for i in 0..n {
        for k in 0..n {
            let aik = a[i * n + k];
            for j in 0..n {
                c[i * n + j] += aik * b[k * n + j];
            }
        }
    }
    let s: f64 = c.iter().sum();
    s as i64
}

fn main() {
    let n: i64 = env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(10);
    println!("{}", matmul_checksum(n));
}
