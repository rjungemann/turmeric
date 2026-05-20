import sys

n = int(sys.argv[1]) if len(sys.argv) > 1 else 1000000

def inc1(x): return x + 1

v = 0
for _ in range(n):
    v = inc1(v)
print(v)
