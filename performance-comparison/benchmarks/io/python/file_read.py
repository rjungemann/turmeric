import time
import os

def measure_time():
    return time.perf_counter()

def read_file(filename):
    with open(filename, 'rb') as f:
        while f.read(4096):
            pass

if __name__ == "__main__":
    import sys
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <file>")
        sys.exit(1)

    start = measure_time()
    read_file(sys.argv[1])
    end = measure_time()

    print(f"Time taken: {end - start:.6f} seconds")