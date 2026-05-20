import sys

n = int(sys.argv[1]) if len(sys.argv) > 1 else 1000
m = {i: i * 2 for i in range(n)}
print(sum(m[i] for i in range(n)))
