import sys
import time
import json

def recursive_fib(n):
    if n <= 1:
        return n
    return recursive_fib(n - 1) + recursive_fib(n - 2)

def iterative_fib(n):
    if n <= 1:
        return n
    a, b = 0, 1
    for _ in range(2, n + 1):
        a, b = b, a + b
    return b

def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <n>")
        sys.exit(1)

    n = int(sys.argv[1])
    if n < 0:
        print("n must be non-negative")
        sys.exit(1)

    # Recursive
    start = time.time()
    result = recursive_fib(n)
    time_ms = (time.time() - start) * 1000

    print(json.dumps({
        "language": "Python",
        "version": f"{sys.version.split()[0]}",
        "implementation": "recursive",
        "n": n,
        "result": result,
        "time_ms": time_ms
    }))

    # Iterative
    start = time.time()
    result = iterative_fib(n)
    time_ms = (time.time() - start) * 1000

    print(json.dumps({
        "language": "Python",
        "version": f"{sys.version.split()[0]}",
        "implementation": "iterative",
        "n": n,
        "result": result,
        "time_ms": time_ms
    }))

if __name__ == "__main__":
    main()