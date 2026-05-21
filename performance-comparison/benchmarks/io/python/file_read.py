import sys, os

n    = int(sys.argv[1]) if len(sys.argv) > 1 else 1048576
path = "/tmp/bench_io_read_py.bin"
with open(path, 'wb') as fw:
    rem = n
    while rem > 0:
        chunk = min(4096, rem)
        fw.write(b'\xCD' * chunk)
        rem -= chunk
total = 0
with open(path, 'rb') as fr:
    while True:
        data = fr.read(4096)
        if not data:
            break
        total += len(data)
os.remove(path)
print(total)
