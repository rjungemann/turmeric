// monte_carlo_pi -- LCG-driven Monte Carlo estimate of pi, %.6f output.
use std::env;

const LCG_A: u64 = 6364136223846793005;
const LCG_C: u64 = 1442695040888963407;

fn lcg_next(state: u64) -> u64 {
    state.wrapping_mul(LCG_A).wrapping_add(LCG_C)
}

fn unit_float(bits: u64) -> f64 {
    // upper 53 bits, mapped to [0, 1)
    ((bits >> 11) as f64) / 9007199254740992.0
}

fn estimate_pi(iters: i64) -> f64 {
    let mut state: u64 = LCG_A;
    let mut inside: i64 = 0;
    let mut i: i64 = 0;
    while i < iters {
        let s1 = lcg_next(state);
        let x = unit_float(s1);
        let s2 = lcg_next(s1);
        let y = unit_float(s2);
        state = s2;
        if x * x + y * y <= 1.0 {
            inside += 1;
        }
        i += 1;
    }
    4.0 * (inside as f64) / (iters as f64)
}

fn main() {
    let n: i64 = env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(1000);
    println!("{:.6}", estimate_pi(n));
}
