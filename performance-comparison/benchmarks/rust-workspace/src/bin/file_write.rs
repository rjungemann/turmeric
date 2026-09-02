// file_write -- write n bytes to a temp file in 4 KiB chunks, remove it,
// print the byte count.
use std::env;
use std::fs;
use std::io::Write;

const CHUNK: usize = 4096;

fn file_write_bench(n: i64) -> i64 {
    let n = n as usize;
    let buf = [171u8; CHUNK]; // 0xAB
    let path = "/tmp/bench_io_write_rs.bin";
    let mut f = fs::File::create(path).unwrap();
    let mut written = 0usize;
    while written < n {
        let chunk = std::cmp::min(CHUNK, n - written);
        f.write_all(&buf[..chunk]).unwrap();
        written += chunk;
    }
    drop(f);
    let _ = fs::remove_file(path);
    written as i64
}

fn main() {
    let n: i64 = env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(1048576);
    println!("{}", file_write_bench(n));
}
