// ray_tracing -- three-sphere diffuse tracer over a width x height image,
// prints the integer checksum. Direct port of the Turmeric/C inline-C
// vector math (add/sub/scale/dot/normalize on 3-tuples).
use std::env;

#[derive(Clone, Copy)]
struct Vec3 {
    x: f64,
    y: f64,
    z: f64,
}

fn add(a: Vec3, b: Vec3) -> Vec3 {
    Vec3 { x: a.x + b.x, y: a.y + b.y, z: a.z + b.z }
}
fn sub(a: Vec3, b: Vec3) -> Vec3 {
    Vec3 { x: a.x - b.x, y: a.y - b.y, z: a.z - b.z }
}
fn scale(v: Vec3, s: f64) -> Vec3 {
    Vec3 { x: v.x * s, y: v.y * s, z: v.z * s }
}
fn dot(a: Vec3, b: Vec3) -> f64 {
    a.x * b.x + a.y * b.y + a.z * b.z
}
fn norm(v: Vec3) -> Vec3 {
    scale(v, 1.0 / (dot(v, v).sqrt() + 1e-15))
}

struct Sphere {
    center: Vec3,
    radius: f64,
}

fn run_raytracer(width: i64, height: i64) -> i64 {
    let spheres = [
        Sphere { center: Vec3 { x: 0.0, y: 0.0, z: -5.0 }, radius: 1.0 },
        Sphere { center: Vec3 { x: 2.0, y: 0.0, z: -7.0 }, radius: 1.5 },
        Sphere { center: Vec3 { x: -3.0, y: 0.0, z: -6.0 }, radius: 0.8 },
    ];
    let light = norm(Vec3 { x: 1.0, y: 1.0, z: -1.0 });
    let origin = Vec3 { x: 0.0, y: 0.0, z: 0.0 };

    let mut checksum: i64 = 0;
    for y in 0..height {
        for x in 0..width {
            let u = (x as f64 / width as f64) * 2.0 - 1.0;
            let v = (y as f64 / height as f64) * 2.0 - 1.0;
            let dir = norm(Vec3 { x: u, y: v, z: -1.0 });

            let mut best = 1e18f64;
            let mut bi: i64 = -1;
            for (idx, s) in spheres.iter().enumerate() {
                let oc = sub(origin, s.center);
                let a = dot(dir, dir);
                let b2 = dot(oc, dir);
                let c = dot(oc, oc) - s.radius * s.radius;
                let d = b2 * b2 - a * c;
                if d >= 0.0 {
                    let t = (-b2 - d.sqrt()) / a;
                    if t > 0.001 && t < best {
                        best = t;
                        bi = idx as i64;
                    }
                }
            }

            if bi >= 0 {
                let s = &spheres[bi as usize];
                let hp = add(origin, scale(dir, best));
                let n = norm(sub(hp, s.center));
                let mut diff = dot(n, light);
                if diff < 0.0 {
                    diff = 0.0;
                }
                checksum += (diff * 255.0) as i64;
            }
        }
    }
    checksum
}

fn main() {
    let args: Vec<String> = env::args().collect();
    let width: i64 = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(100);
    let height: i64 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(75);
    println!("{}", run_raytracer(width, height));
}
