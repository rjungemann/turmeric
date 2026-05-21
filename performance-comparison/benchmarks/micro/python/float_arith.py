import sys, math

n = int(sys.argv[1]) if len(sys.argv) > 1 else 1000000
a, b = 1.0, 1.0
for _ in range(n):
    a, b = a * 1.0000001 + math.sqrt(b), b * 0.9999999 + math.sqrt(a)
print(f"{a + b:.6f}")
