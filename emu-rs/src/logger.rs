//! UART exchange logger for real-motor sessions.
//!
//! When the emulator talks to a real motor (`--motor-port`, `--motor-fd`,
//! `--motor-usb`) every byte in each direction is appended to
//! `logs/<date_time>.log` for later protocol analysis. Inactive for the
//! built-in BBSHD backend (that traffic is already known).
//!
//! The Bafang protocol is request/response: the display writes a short request
//! (TX) and reads the reply (RX). We buffer RX bytes and flush them as one line
//! just before the next TX, so each reply lands on its own line in order.

use std::fs::{create_dir_all, File};
use std::io::Write;
use std::sync::Mutex;
use std::time::Instant;

struct Logger {
    file: File,
    start: Instant,
    rx: Vec<u8>,           // reply bytes accumulated since the last TX
    rx_at: Option<f64>,    // timestamp (ms) of the first buffered RX byte
    last_tx_op: [u8; 2],   // category+opcode of the last TX, to label the reply
}

static LOGGER: Mutex<Option<Logger>> = Mutex::new(None);

/// Start logging this session to `logs/<date_time>.log`. Best-effort: on any
/// filesystem error it prints a warning and stays inactive.
pub fn init(port: &str) {
    if create_dir_all("logs").is_err() {
        eprintln!("swang-stodva-emu: cannot create logs/ — UART logging disabled");
        return;
    }
    let path = format!("logs/{}.log", timestamp_name());
    let mut file = match File::create(&path) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("swang-stodva-emu: cannot open {path}: {e} — UART logging disabled");
            return;
        }
    };
    let _ = writeln!(
        file,
        "# swang-stodva-emu UART log\n\
         # port: {port}\n\
         # started: {}\n\
         # t(ms) relative to start; TX = display->motor, RX = motor->display",
        human_time()
    );
    eprintln!("swang-stodva-emu: logging UART exchange to {path}");
    *LOGGER.lock().unwrap() = Some(Logger {
        file,
        start: Instant::now(),
        rx: Vec::new(),
        rx_at: None,
        last_tx_op: [0, 0],
    });
}

/// Log an outbound request. Flushes any buffered reply to the previous request
/// first, so log order matches wire order.
pub fn log_tx(bytes: &[u8]) {
    if let Ok(mut g) = LOGGER.lock() {
        if let Some(l) = g.as_mut() {
            l.flush_rx();
            let t = l.start.elapsed().as_secs_f64() * 1000.0;
            let _ = writeln!(l.file, "[{t:9.1}] TX {:<20} {}", hex(bytes), decode_tx(bytes));
            if bytes.len() >= 2 {
                l.last_tx_op = [bytes[0], bytes[1]];
            }
        }
    }
}

/// Log one inbound reply byte (buffered until the next TX or flush()).
pub fn log_rx_byte(b: u8) {
    if let Ok(mut g) = LOGGER.lock() {
        if let Some(l) = g.as_mut() {
            if l.rx.is_empty() {
                l.rx_at = Some(l.start.elapsed().as_secs_f64() * 1000.0);
            }
            l.rx.push(b);
        }
    }
}

/// Flush any pending reply bytes (call before exit).
pub fn flush() {
    if let Ok(mut g) = LOGGER.lock() {
        if let Some(l) = g.as_mut() {
            l.flush_rx();
            let _ = l.file.flush();
        }
    }
}

impl Logger {
    fn flush_rx(&mut self) {
        if self.rx.is_empty() {
            return;
        }
        let t = self.rx_at.unwrap_or(0.0);
        let reply_to = decode_op(self.last_tx_op[0], self.last_tx_op[1]);
        let _ = writeln!(
            self.file,
            "[{t:9.1}] RX {:<20} (reply to {reply_to})",
            hex(&self.rx)
        );
        self.rx.clear();
        self.rx_at = None;
    }
}

fn hex(bytes: &[u8]) -> String {
    bytes
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect::<Vec<_>>()
        .join(" ")
}

fn decode_tx(b: &[u8]) -> String {
    if b.len() < 2 {
        return String::new();
    }
    let name = decode_op(b[0], b[1]);
    if b.len() > 2 {
        format!("{name} data={}", hex(&b[2..]))
    } else {
        name
    }
}

fn decode_op(cat: u8, op: u8) -> String {
    let name = match (cat, op) {
        (0x11, 0x08) => "READ_STATUS",
        (0x11, 0x0a) => "READ_CURRENT",
        (0x11, 0x11) => "READ_BATTERY",
        (0x11, 0x20) => "READ_SPEED",
        (0x11, 0x21) => "READ_UNKNOWN1",
        (0x11, 0x22) => "READ_RANGE",
        (0x11, 0x24) => "READ_CALORIES",
        (0x11, 0x25) => "READ_UNKNOWN3",
        (0x11, 0x31) => "READ_MOVING",
        (0x16, 0x0b) => "WRITE_PAS",
        (0x16, 0x0c) => "WRITE_MODE",
        (0x16, 0x1a) => "WRITE_LIGHTS",
        (0x16, 0x1f) => "WRITE_SPEED_LIM",
        _ => return format!("{cat:02x}:{op:02x}?"),
    };
    name.to_string()
}

// ---- time formatting via libc (no extra crate) -----------------------------

fn broken_down() -> libc::tm {
    unsafe {
        let t = libc::time(std::ptr::null_mut());
        let mut tm: libc::tm = std::mem::zeroed();
        libc::localtime_r(&t, &mut tm);
        tm
    }
}

fn strftime(fmt: &[u8]) -> String {
    let tm = broken_down();
    let mut buf = [0u8; 64];
    let n = unsafe {
        libc::strftime(
            buf.as_mut_ptr() as *mut libc::c_char,
            buf.len(),
            fmt.as_ptr() as *const libc::c_char,
            &tm,
        )
    };
    String::from_utf8_lossy(&buf[..n]).into_owned()
}

fn timestamp_name() -> String {
    strftime(b"%Y-%m-%d_%H-%M-%S\0")
}

fn human_time() -> String {
    strftime(b"%Y-%m-%d %H:%M:%S\0")
}
