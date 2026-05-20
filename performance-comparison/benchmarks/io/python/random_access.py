import time
import random

def measure_time():
    return time.perf_counter()

def random_access(filename):
    num_access = 100000
    file_size = 100 * 1024 * 1024  # 100MB

    with open(filename, 'rb') as f:
        for _ in range(num_access):
            offset = random.randint(0, file_size - 1)
            f.seek(offset)
            f.read(1)

if __name__ == "__main__":
    import sys
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <file>")
        sys.exit(1)

    start = measure_time()
    random_access(sys.argv[1])
    end = measure_time()

    print(f"Time taken: {end - start:.6f} seconds")