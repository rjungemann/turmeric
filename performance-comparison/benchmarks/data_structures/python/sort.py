import sys

n = int(sys.argv[1]) if len(sys.argv) > 1 else 1000
mask = (1 << 64) - 1
s = 12345
arr = []
for _ in range(n):
    s = (s * 6364136223846793005 + 1442695040888963407) & mask
    arr.append(s >> 1)
arr.sort()
print(arr[0], arr[-1])
