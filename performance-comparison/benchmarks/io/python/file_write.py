import sys, os

n    = int(sys.argv[1]) if len(sys.argv) > 1 else 1048576
path = "/tmp/bench_io_write_py.bin"
buf  = b'\xAB' * 4096
with open(path, 'wb') as f:
    written = 0
    while written < n:
        chunk = min(4096, n - written)
        f.write(buf[:chunk])
        written += chunk
os.remove(path)
print(n)
