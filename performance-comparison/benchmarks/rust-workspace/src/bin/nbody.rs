// nbody -- LCG-initialized bodies, pairwise force simulation, kinetic
// energy checksum at %.4f. Init casts the LCG's top 32 bits through
// signed 64-bit, like the C/Turmeric/Haskell columns.
use std::env;

const LCG_A: u64 = 6364136223846793005;
const LCG_C: u64 = 1442695040888963407;

#[derive(Clone, Copy)]
struct Body {
    x: f64,
    y: f64,
    z: f64,
    vx: f64,
    vy: f64,
    vz: f64,
    mass: f64,
}

fn lcg_f(s: u64) -> f64 {
    ((s >> 32) as i64) as f64 / 1e8
}

fn run_nbody(n_bodies: i64, steps: i64) -> f64 {
    let n = n_bodies as usize;
    let mut bodies: Vec<Body> = Vec::with_capacity(n);
    let mut state: u64 = 42;
    for i in 0..n {
        state = state.wrapping_mul(LCG_A).wrapping_add(LCG_C);
        let x = lcg_f(state);
        state = state.wrapping_mul(LCG_A).wrapping_add(LCG_C);
        let y = lcg_f(state);
        state = state.wrapping_mul(LCG_A).wrapping_add(LCG_C);
        let z = lcg_f(state);
        bodies.push(Body {
            x,
            y,
            z,
            vx: 0.0,
            vy: 0.0,
            vz: 0.0,
            mass: 1.0 + (i % 5) as f64 * 0.5,
        });
    }

    for _ in 0..steps {
        for i in 0..n {
            for j in (i + 1)..n {
                let dx = bodies[j].x - bodies[i].x;
                let dy = bodies[j].y - bodies[i].y;
                let dz = bodies[j].z - bodies[i].z;
                let dist = (dx * dx + dy * dy + dz * dz).sqrt() + 1e-10;
                let f = bodies[i].mass * bodies[j].mass / (dist * dist * dist);
                bodies[i].vx += f * dx;
                bodies[i].vy += f * dy;
                bodies[i].vz += f * dz;
                bodies[j].vx -= f * dx;
                bodies[j].vy -= f * dy;
                bodies[j].vz -= f * dz;
            }
        }
        for b in bodies.iter_mut() {
            b.x += b.vx;
            b.y += b.vy;
            b.z += b.vz;
        }
    }

    let mut ke = 0.0;
    for b in bodies.iter() {
        ke += 0.5 * b.mass * (b.vx * b.vx + b.vy * b.vy + b.vz * b.vz);
    }
    ke
}

fn main() {
    let args: Vec<String> = env::args().collect();
    let n_bodies: i64 = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(5);
    let steps: i64 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(1000);
    println!("{:.4}", run_nbody(n_bodies, steps));
}
