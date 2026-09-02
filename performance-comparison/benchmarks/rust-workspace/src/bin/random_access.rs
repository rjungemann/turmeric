// random_access -- write file_size bytes of a sequential 0..255 pattern,
// then do n_reads LCG-driven single-byte seeks, print the checksum.
use std::env;
use std::fs;
use std::io::{Read, Seek, SeekFrom, Write};

const LCG_A: u64 = 6364136223846793005;
const LCG_C: u64 = 1442695040888963407;
const CHUNK: usize = 4096;

fn random_access_bench(file_size: i64, n_reads: i64) -> i64 {
    let file_size = file_size as u64;
    let path = "/tmp/bench_io_random_rs.bin";
    {
        let mut f = fs::File::create(path).unwrap();
        let mut rem = file_size;
        let mut seq: u32 = 0;
        let mut wbuf = [0u8; CHUNK];
        while rem > 0 {
            let chunk = std::cmp::min(CHUNK as u64, rem) as usize;
            for b in wbuf.iter_mut().take(chunk) {
                *b = (seq & 0xFF) as u8;
                seq = seq.wrapping_add(1);
            }
            f.write_all(&wbuf[..chunk]).unwrap();
            rem -= chunk as u64;
        }
    }
    let mut f = fs::File::open(path).unwrap();
    let mut state: u64 = 12345678;
    let mut checksum: i64 = 0;
    let mut byte = [0u8; 1];
    for _ in 0..n_reads {
        state = state.wrapping_mul(LCG_A).wrapping_add(LCG_C);
        let offset = (state >> 1) % file_size;
        f.seek(SeekFrom::Start(offset)).unwrap();
        if f.read(&mut byte).unwrap() == 1 {
            checksum += byte[0] as i64;
        }
    }
    drop(f);
    let _ = fs::remove_file(path);
    checksum
}

fn main() {
    let args: Vec<String> = env::args().collect();
    let file_size: i64 = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(1048576);
    let n_reads: i64 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(1000);
    println!("{}", random_access_bench(file_size, n_reads));
}
