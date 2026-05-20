import sys

n = int(sys.argv[1]) if len(sys.argv) > 1 else 1000
s = "hello" * n
print(len(s))
