// text_search -- generate an x/hello haystack, count non-overlapping
// "hello" occurrences over raw bytes (like the C strstr loop).
use std::env;

fn search_bench(hs_size: i64) -> i64 {
    let needle: &[u8] = b"hello";
    let nlen = needle.len();
    let hs_size = hs_size as usize;
    let mut hay: Vec<u8> = Vec::with_capacity(hs_size);
    for i in 0..hs_size {
        let m = i % 10;
        hay.push(if m < 5 { b'x' } else { needle[m - 5] });
    }
    let mut count: i64 = 0;
    let mut pos = 0usize;
    while pos + nlen <= hay.len() {
        if &hay[pos..pos + nlen] == needle {
            count += 1;
            pos += nlen;
        } else {
            pos += 1;
        }
    }
    count
}

fn main() {
    let n: i64 = env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(10000);
    println!("{}", search_bench(n));
}
