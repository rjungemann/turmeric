// thread_ring -- pass a decrementing token around n_threads real OS
// threads until it reaches zero, mirroring the pthread mutex/cond ring in
// the Turmeric/C column with std::thread + mpsc channels.
use std::env;
use std::sync::mpsc;
use std::thread;

fn run_ring(n_threads: i64, messages: i64) {
    let n = n_threads as usize;

    let mut senders: Vec<mpsc::Sender<i64>> = Vec::with_capacity(n);
    let mut receivers: Vec<Option<mpsc::Receiver<i64>>> = Vec::with_capacity(n);
    for _ in 0..n {
        let (tx, rx) = mpsc::channel();
        senders.push(tx);
        receivers.push(Some(rx));
    }

    let mut handles = Vec::with_capacity(n);
    for i in 0..n {
        let rx = receivers[i].take().unwrap();
        let next_tx = senders[(i + 1) % n].clone();
        handles.push(thread::spawn(move || loop {
            let tok = match rx.recv() {
                Ok(t) => t,
                Err(_) => return,
            };
            let out = if tok > 0 { tok - 1 } else { tok };
            let _ = next_tx.send(out);
            if tok <= 0 {
                return;
            }
        }));
    }

    let _ = senders[0].send(messages);
    for h in handles {
        let _ = h.join();
    }
    println!("done");
}

fn main() {
    let args: Vec<String> = env::args().collect();
    let n_threads: i64 = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(4);
    let messages: i64 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(1000);
    run_ring(n_threads, messages);
}
