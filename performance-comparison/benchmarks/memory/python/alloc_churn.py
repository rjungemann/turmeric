import sys

n = int(sys.argv[1]) if len(sys.argv) > 1 else 10000
total = 0
for i in range(n):
    obj = [i]   # allocate a small list object
    total += obj[0]
print(total)
