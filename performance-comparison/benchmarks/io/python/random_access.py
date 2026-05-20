import sys, os

file_size = int(sys.argv[1]) if len(sys.argv) > 1 else 1048576
n_reads   = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
path      = "/tmp/bench_io_random_py.bin"
MASK      = (1 << 64) - 1

with open(path, 'wb') as fw:
    pos = 0
    while pos < file_size:
        chunk = min(4096, file_size - pos)
        fw.write(bytes((pos + i) & 0xFF for i in range(chunk)))
        pos += chunk

state, checksum = 12345678, 0
with open(path, 'rb') as fr:
    for _ in range(n_reads):
        state = (state * 6364136223846793005 + 1442695040888963407) & MASK
        fr.seek((state >> 1) % file_size)
        b = fr.read(1)
        if b: checksum += b[0]
os.remove(path)
print(checksum)
