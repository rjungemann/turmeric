import sys

def estimate_pi(iters):
    state = 6364136223846793005
    mask = (1 << 64) - 1
    inside = 0
    for _ in range(iters):
        state = (state * 6364136223846793005 + 1442695040888963407) & mask
        x = (state >> 11) / (1 << 53)
        state = (state * 6364136223846793005 + 1442695040888963407) & mask
        y = (state >> 11) / (1 << 53)
        if x * x + y * y <= 1.0:
            inside += 1
    return 4.0 * inside / iters

n = int(sys.argv[1]) if len(sys.argv) > 1 else 1000
print(f"{estimate_pi(n):.6f}")
