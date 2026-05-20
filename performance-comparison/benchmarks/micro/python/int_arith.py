import sys

n = int(sys.argv[1]) if len(sys.argv) > 1 else 1000000
a, b = 1, 1
for _ in range(n):
    a, b = a * 1000003 + b, b * 999983 + a
print(a ^ b)
