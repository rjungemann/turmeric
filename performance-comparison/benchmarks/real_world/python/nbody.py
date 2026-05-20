import time
import math
import random

NUM_BODIES = 5
STEPS = 10000

class Body:
    def __init__(self, x, y, z, vx, vy, vz, mass):
        self.x = x
        self.y = y
        self.z = z
        self.vx = vx
        self.vy = vy
        self.vz = vz
        self.mass = mass

def measure_time():
    return time.perf_counter()

def update_bodies(bodies):
    # Calculate forces
    fx = [0.0] * NUM_BODIES
    fy = [0.0] * NUM_BODIES
    fz = [0.0] * NUM_BODIES

    for i in range(NUM_BODIES):
        for j in range(i + 1, NUM_BODIES):
            dx = bodies[j].x - bodies[i].x
            dy = bodies[j].y - bodies[i].y
            dz = bodies[j].z - bodies[i].z
            dist_sq = dx * dx + dy * dy + dz * dz
            dist = math.sqrt(dist_sq)
            force = bodies[i].mass * bodies[j].mass / dist_sq

            fx[i] += force * dx / dist
            fy[i] += force * dy / dist
            fz[i] += force * dz / dist

            fx[j] -= force * dx / dist
            fy[j] -= force * dy / dist
            fz[j] -= force * dz / dist

    # Update positions and velocities
    for i in range(NUM_BODIES):
        bodies[i].vx += fx[i] / bodies[i].mass
        bodies[i].vy += fy[i] / bodies[i].mass
        bodies[i].vz += fz[i] / bodies[i].mass

        bodies[i].x += bodies[i].vx
        bodies[i].y += bodies[i].vy
        bodies[i].z += bodies[i].vz

def main():
    random.seed(42)
    bodies = []

    # Initialize bodies with random positions and velocities
    for _ in range(NUM_BODIES):
        x = random.random() * 100 - 50
        y = random.random() * 100 - 50
        z = random.random() * 100 - 50
        vx = random.random() * 2 - 1
        vy = random.random() * 2 - 1
        vz = random.random() * 2 - 1
        mass = random.random() * 10 + 1
        bodies.append(Body(x, y, z, vx, vy, vz, mass))

    start = measure_time()

    for _ in range(STEPS):
        update_bodies(bodies)

    end = measure_time()
    print(f"Time taken: {end - start:.6f} seconds")

if __name__ == "__main__":
    main()