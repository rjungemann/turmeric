import sys, math

width  = int(sys.argv[1]) if len(sys.argv) > 1 else 100
height = int(sys.argv[2]) if len(sys.argv) > 2 else 75

spheres = [(0, 0, -5, 1.0), (2, 0, -7, 1.5), (-3, 0, -6, 0.8)]

def dot(a, b): return sum(x*y for x,y in zip(a,b))
def vnorm(v):
    l = math.sqrt(dot(v,v)) + 1e-15
    return tuple(x/l for x in v)
def vsub(a, b): return tuple(x-y for x,y in zip(a,b))
def vadd(a, b): return tuple(x+y for x,y in zip(a,b))
def vscale(v, s): return tuple(x*s for x in v)

light = vnorm((1, 1, -1))

def sphere_hit(cx, cy, cz, r, ox, oy, oz, dx, dy, dz):
    oc = (ox-cx, oy-cy, oz-cz)
    rd = (dx, dy, dz)
    a = dot(rd, rd); b2 = dot(oc, rd); c = dot(oc, oc) - r*r
    d = b2*b2 - a*c
    if d < 0: return None
    t = (-b2 - math.sqrt(d)) / a
    return t if t > 0.001 else None

checksum = 0
for y in range(height):
    for x in range(width):
        u = (x / width)  * 2 - 1
        v = (y / height) * 2 - 1
        rd = vnorm((u, v, -1))
        best_t, best_s = None, None
        for s in spheres:
            t = sphere_hit(s[0], s[1], s[2], s[3], 0, 0, 0, *rd)
            if t is not None and (best_t is None or t < best_t):
                best_t, best_s = t, s
        if best_s:
            hp = vscale(rd, best_t)
            n  = vnorm(vsub(hp, (best_s[0], best_s[1], best_s[2])))
            diff = max(0.0, dot(n, light))
            checksum += int(diff * 255)
print(checksum)
