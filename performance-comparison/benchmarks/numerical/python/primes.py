import sys

def count_primes(limit):
    sieve = bytearray(limit + 1)
    count = 0
    for i in range(2, limit + 1):
        if not sieve[i]:
            count += 1
            for j in range(2 * i, limit + 1, i):
                sieve[j] = 1
    return count

n = int(sys.argv[1]) if len(sys.argv) > 1 else 10000
print(count_primes(n))
