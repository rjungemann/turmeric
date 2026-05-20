import sys

MOD = 1_000_000_007

def factorial(n, acc=1):
    if n <= 1:
        return acc % MOD
    return factorial(n - 1, (acc * n) % MOD)

n = int(sys.argv[1]) if len(sys.argv) > 1 else 1000
sys.setrecursionlimit(n + 100)
print(factorial(n))
