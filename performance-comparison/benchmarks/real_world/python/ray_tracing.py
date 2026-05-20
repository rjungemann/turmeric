import time
import math
import random

WIDTH = 800
HEIGHT = 600
MAX_DEPTH = 5

class Vec3:
    def __init__(self, x, y, z):
        self.x = x
        self.y = y
        self.z = z

    def __add__(self, other):
        return Vec3(self.x + other.x, self.y + other.y, self.z + other.z)

    def __sub__(self, other):
        return Vec3(self.x - other.x, self.y - other.y, self.z - other.z)

    def __mul__(self, scalar):
        return Vec3(self.x * scalar, self.y * scalar, self.z * scalar)

    def dot(self, other):
        return self.x * other.x + self.y * other.y + self.z * other.z

    def length(self):
        return math.sqrt(self.dot(self))

    def normalize(self):
        return self * (1.0 / self.length())

class Sphere:
    def __init__(self, center, radius):
        self.center = center
        self.radius = radius

class Ray:
    def __init__(self, origin, direction):
        self.origin = origin
        self.direction = direction

def sphere_intersect(sphere, ray):
    oc = ray.origin - sphere.center
    a = ray.direction.dot(ray.direction)
    b = 2.0 * oc.dot(ray.direction)
    c = oc.dot(oc) - sphere.radius * sphere.radius
    discriminant = b * b - 4 * a * c

    if discriminant < 0:
        return None

    sqrt_disc = math.sqrt(discriminant)
    t0 = (-b - sqrt_disc) / (2.0 * a)
    t1 = (-b + sqrt_disc) / (2.0 * a)

    return min(t0, t1)

def trace(ray, spheres, depth):
    if depth > MAX_DEPTH:
        return Vec3(0, 0, 0)

    closest_t = float('inf')
    closest_idx = -1

    for i, sphere in enumerate(spheres):
        t = sphere_intersect(sphere, ray)
        if t is not None and t < closest_t:
            closest_t = t
            closest_idx = i

    if closest_idx == -1:
        return Vec3(0, 0, 0)

    hit_point = ray.origin + ray.direction * closest_t
    normal = (hit_point - spheres[closest_idx].center).normalize()

    # Simple diffuse lighting
    light_dir = Vec3(1, 1, -1).normalize()
    diffuse = max(0, normal.dot(light_dir))

    return Vec3(diffuse, diffuse, diffuse) * 255

def measure_time():
    return time.perf_counter()

def main():
    spheres = [
        Sphere(Vec3(0, 0, -5), 1),
        Sphere(Vec3(2, 0, -7), 1.5),
        Sphere(Vec3(-3, 0, -6), 0.8)
    ]

    start = measure_time()

    for y in range(HEIGHT):
        for x in range(WIDTH):
            u = x / WIDTH
            v = y / HEIGHT

            ray = Ray(
                Vec3(0, 0, 0),
                Vec3(u * 2 - 1, v * 2 - 1, -1).normalize()
            )

            color = trace(ray, spheres, 0)
            # In a real implementation, we would write the pixel color

    end = measure_time()
    print(f"Time taken: {end - start:.6f} seconds")

if __name__ == "__main__":
    main()