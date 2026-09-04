// file_read -- write n bytes (0xCD) then read them back in 4 KiB chunks,
// print bytes read.
use std::env;
use std::fs;
use std::io::{Read, Write};

const CHUNK: usize = 4096;

fn file_read_bench(n: i64) -> i64 {
    let n = n as usize;
    let path = "/tmp/bench_io_read_rs.bin";
    let buf = [0xCDu8; CHUNK];
    {
        let mut f = fs::File::create(path).unwrap();
        let mut rem = n;
        while rem > 0 {
            let chunk = std::cmp::min(CHUNK, rem);
            f.write_all(&buf[..chunk]).unwrap();
            rem -= chunk;
        }
    }
    let mut f = fs::File::open(path).unwrap();
    let mut rbuf = [0u8; CHUNK];
    let mut total: i64 = 0;
    loop {
        let nr = f.read(&mut rbuf).unwrap();
        if nr == 0 {
            break;
        }
        total += nr as i64;
    }
    drop(f);
    let _ = fs::remove_file(path);
    total
}

fn main() {
    let n: i64 = env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(1048576);
    println!("{}", file_read_bench(n));
}
