import sys, math

n_bodies = int(sys.argv[1]) if len(sys.argv) > 1 else 5
steps    = int(sys.argv[2]) if len(sys.argv) > 2 else 1000

MASK = (1 << 64) - 1
A, C = 6364136223846793005, 1442695040888963407

def lcg(s): return (s * A + C) & MASK
def lcg_float(s): return (s >> 32) / 1e8

state = 42
bodies = []
for i in range(n_bodies):
    s1 = lcg(state); s2 = lcg(s1); s3 = lcg(s2); state = s3
    bodies.append([lcg_float(s1), lcg_float(s2), lcg_float(s3),
                   0.0, 0.0, 0.0, 1.0 + (i % 5) * 0.5])

for _ in range(steps):
    for i in range(n_bodies):
        for j in range(i + 1, n_bodies):
            bi, bj = bodies[i], bodies[j]
            dx, dy, dz = bj[0]-bi[0], bj[1]-bi[1], bj[2]-bi[2]
            dist = math.sqrt(dx*dx + dy*dy + dz*dz) + 1e-10
            f = bi[6] * bj[6] / (dist * dist * dist)
            bi[3] += f*dx; bi[4] += f*dy; bi[5] += f*dz
            bj[3] -= f*dx; bj[4] -= f*dy; bj[5] -= f*dz
    for b in bodies:
        b[0] += b[3]; b[1] += b[4]; b[2] += b[5]

ke = sum(0.5 * b[6] * (b[3]**2 + b[4]**2 + b[5]**2) for b in bodies)
print(f"{ke:.4f}")
