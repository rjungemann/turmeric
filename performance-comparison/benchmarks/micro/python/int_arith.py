import sys

n = int(sys.argv[1]) if len(sys.argv) > 1 else 1000000
MASK = 0xFFFFFFFFFFFFFFFF
a, b = 1, 1
for _ in range(n):
    a = (a * 1000003 + b) & MASK
    b = (b * 999983 + a) & MASK
result = a ^ b
# Match C's printf("%lld"): convert unsigned 64-bit to signed
if result >= (1 << 63):
    result -= (1 << 64)
print(result)
