import sys

def fib(n):
    if n <= 1:
        return n
    a, b = 0, 1
    for _ in range(2, n + 1):
        a, b = b, a + b
    return b

n = int(sys.argv[1]) if len(sys.argv) > 1 else 1000
print(fib(n))
