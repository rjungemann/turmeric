import sys

hs_size = int(sys.argv[1]) if len(sys.argv) > 1 else 10000
needle = "hello"
hay = "".join("x" if i % 10 < 5 else needle[i % 10 - 5] for i in range(hs_size))
count = 0
pos = 0
while True:
    idx = hay.find(needle, pos)
    if idx < 0:
        break
    count += 1
    pos = idx + len(needle)
print(count)
