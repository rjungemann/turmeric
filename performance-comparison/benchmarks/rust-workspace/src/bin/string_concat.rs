// string_concat -- append "hello" n times, print the final byte length.
// Byte-oriented (Vec<u8>), matching the Turmeric/C/Haskell columns.
use std::env;

fn concat_bench(n: i64) -> usize {
    let piece: &[u8] = b"hello";
    let mut buf: Vec<u8> = Vec::new();
    for _ in 0..n {
        buf.extend_from_slice(piece);
    }
    buf.len()
}

fn main() {
    let n: i64 = env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(1000);
    println!("{}", concat_bench(n));
}
