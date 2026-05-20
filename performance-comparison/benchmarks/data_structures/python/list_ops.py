import sys

n = int(sys.argv[1]) if len(sys.argv) > 1 else 1000
lst = list(range(n))
print(sum(lst))
