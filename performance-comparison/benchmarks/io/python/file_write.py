import time

def measure_time():
    return time.perf_counter()

def write_file(filename):
    total_size = 100 * 1024 * 1024  # 100MB
    buffer = b'x' * 4096
    remaining = total_size

    with open(filename, 'wb') as f:
        while remaining > 0:
            to_write = min(remaining, 4096)
            f.write(buffer[:to_write])
            remaining -= to_write

if __name__ == "__main__":
    import sys
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <file>")
        sys.exit(1)

    start = measure_time()
    write_file(sys.argv[1])
    end = measure_time()

    print(f"Time taken: {end - start:.6f} seconds")