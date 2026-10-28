//! M3 probe: proves that Rust `std` works on RTEMS on the Pluto. Called from the C shell
//! (`nexus_rust_main`) in its own POSIX thread; reports on the console and serves
//! `GET /` on TCP 8080 and an echo on UDP 7000.

use std::io::{Read, Write};
use std::net::{TcpListener, UdpSocket};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, mpsc};
use std::thread;
use std::time::{Duration, Instant};

static UDP_ECHOED: AtomicU64 = AtomicU64::new(0);

fn thread_probe() -> String {
    let t0 = Instant::now();
    let (tx, rx) = mpsc::channel::<u64>();
    let handles: Vec<_> = (0..4)
        .map(|i| {
            let tx = tx.clone();
            thread::Builder::new()
                .name(format!("probe{i}"))
                .stack_size(64 * 1024)
                .spawn(move || {
                    let mut acc = 0u64;
                    for k in 0..2_000_000u64 {
                        acc = acc.wrapping_mul(6364136223846793005).wrapping_add(k ^ i);
                    }
                    tx.send(acc).unwrap();
                })
                .expect("spawn")
        })
        .collect();
    drop(tx);
    for h in handles {
        h.join().unwrap();
    }
    let sum: u64 = rx.iter().fold(0, |a, b| a ^ b);
    let slept = Instant::now();
    thread::sleep(Duration::from_millis(20));
    format!(
        "4 threads in {:.3} ms (xor {:016x}), sleep(20 ms) took {:.3} ms",
        t0.elapsed().as_secs_f64() * 1e3,
        sum,
        slept.elapsed().as_secs_f64() * 1e3
    )
}

fn udp_echo() {
    let sock = match UdpSocket::bind("0.0.0.0:7000") {
        Ok(s) => s,
        Err(e) => return println!("rust: udp bind: {e}"),
    };
    let mut buf = [0u8; 1500];
    loop {
        if let Ok((n, peer)) = sock.recv_from(&mut buf) {
            let _ = sock.send_to(&buf[..n], peer);
            UDP_ECHOED.fetch_add(1, Ordering::Relaxed);
        }
    }
}

fn tcp_status(report: Arc<String>, start: Instant) {
    let listener = match TcpListener::bind("0.0.0.0:8080") {
        Ok(l) => l,
        Err(e) => return println!("rust: tcp bind: {e}"),
    };
    for stream in listener.incoming().flatten() {
        let mut s = stream;
        let mut req = [0u8; 512];
        let _ = s.read(&mut req);
        let body = format!(
            "rust std on RTEMS: ok\nuptime {:.3} s\n{}\nudp echoed {}\n",
            start.elapsed().as_secs_f64(),
            report,
            UDP_ECHOED.load(Ordering::Relaxed)
        );
        let _ = write!(
            s,
            "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: {}\r\n\r\n{}",
            body.len(),
            body
        );
    }
}

#[no_mangle]
pub extern "C" fn nexus_rust_main() -> i32 {
    let start = Instant::now();
    let report = Arc::new(thread_probe());
    println!("rust: std ok: {report}");
    thread::Builder::new()
        .name("udp-echo".into())
        .stack_size(32 * 1024)
        .spawn(udp_echo)
        .expect("udp thread");
    let r = report.clone();
    thread::Builder::new()
        .name("tcp-status".into())
        .stack_size(64 * 1024)
        .spawn(move || tcp_status(r, start))
        .expect("tcp thread");
    0
}
