//! Built-in BBSHD motor emulation.
//!
//! Used when the emulator starts without a real motor port (`--motor-port`):
//! the firmware's UART is bridged to this in-process model instead of a pty.
//! It speaks the same Bafang display protocol as `tools/bbshd_mock.py` — the
//! display polls a round-robin of READ opcodes and writes PAS/lights back.
//!
//! Single-threaded: the FFI serial hooks (called from the firmware C during a
//! tick) and the UI (read/adjust) all run on the emulator's one thread, so the
//! Mutex is uncontended.

use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{LazyLock, Mutex};

static ACTIVE: AtomicBool = AtomicBool::new(false);
static MOTOR: LazyLock<Mutex<Bbshd>> = LazyLock::new(|| Mutex::new(Bbshd::new()));

const PERIMETER_MM: u32 = 2100; // matches the firmware's default wheel perimeter

struct Bbshd {
    speed_kph_x10: u16, // wheel speed, 0.1 km/h units (user-controlled)
    battery_v_x10: u16, // pack voltage, 0.1 V units (user-controlled)
    pas_wire: u8,       // last WRITE_PAS wire code the display sent
    lights: bool,       // last WRITE_LIGHTS state
    motor_temp_c: u8,
    rx: Vec<u8>,        // request bytes from the display, awaiting parse
    tx: VecDeque<u8>,   // reply bytes queued for the display
}

impl Bbshd {
    fn new() -> Self {
        Bbshd {
            speed_kph_x10: 0,
            battery_v_x10: 520, // 52.0 V (14S, ~60%)
            pas_wire: 0,
            lights: false,
            motor_temp_c: 24,
            rx: Vec::new(),
            tx: VecDeque::new(),
        }
    }

    fn moving(&self) -> bool {
        self.speed_kph_x10 > 0
    }

    fn wheel_rpm(&self) -> u16 {
        // rpm = kph * 1e6 / (perimeter_mm * 60); speed is in 0.1 km/h units
        ((self.speed_kph_x10 as u32 * 100_000) / (PERIMETER_MM * 60)) as u16
    }

    fn amp_x2(&self) -> u8 {
        // cosmetic: a little battery current while moving, scaled with speed
        if self.moving() {
            ((self.speed_kph_x10 as u32 * 8) / 100).min(255) as u8
        } else {
            0
        }
    }

    fn battery_pct(&self) -> u8 {
        // 14S li-ion: 42.0 V empty .. 58.8 V full
        (((self.battery_v_x10 as i32 - 420) * 100) / 168).clamp(0, 100) as u8
    }

    // Parse display requests and queue replies. Framing has no delimiters; the
    // opcode determines the length (reads are 2 bytes, writes 3-5).
    fn feed(&mut self, bytes: &[u8]) {
        self.rx.extend_from_slice(bytes);
        let mut i = 0;
        while i < self.rx.len() {
            match self.rx[i] {
                0x11 => {
                    // READ: [cat, op]
                    if i + 2 > self.rx.len() {
                        break;
                    }
                    let op = self.rx[i + 1];
                    i += 2;
                    self.reply(op);
                }
                0x16 => {
                    // WRITE: [cat, op, data..]
                    if i + 2 > self.rx.len() {
                        break;
                    }
                    let op = self.rx[i + 1];
                    let len = write_len(op);
                    if len == 0 {
                        i += 1; // unknown opcode -> resync
                        continue;
                    }
                    if i + len > self.rx.len() {
                        break; // wait for the rest of the frame
                    }
                    let data = self.rx[i + 2];
                    i += len;
                    match op {
                        0x0B => self.pas_wire = data,       // WRITE_PAS
                        0x1A => self.lights = data == 0xF1, // WRITE_LIGHTS (0xF1 = on)
                        _ => {}                             // MODE / SPEED_LIM: ignored
                    }
                }
                _ => i += 1, // stray byte -> resync
            }
        }
        self.rx.drain(0..i);
    }

    fn reply(&mut self, op: u8) {
        match op {
            0x08 => self.tx.push_back(0x00), // STATUS: normal
            0x0A => {
                let a = self.amp_x2();
                self.push(&[a, a]); // CURRENT (degenerate checksum)
            }
            0x11 => {
                let p = self.battery_pct();
                self.push(&[p, p]); // BATTERY %
            }
            0x20 => {
                let (hi, lo) = split(self.wheel_rpm());
                self.push(&[hi, lo, hi.wrapping_add(lo).wrapping_add(0x20)]); // SPEED
            }
            0x21 => self.push(&[0, 0, 0]),
            0x22 => {
                let (hi, lo) = split(self.motor_temp_c as u16);
                self.push(&[hi, lo, hi.wrapping_add(lo)]); // RANGE hijack: motor temp
            }
            0x24 => {
                let (hi, lo) = split(self.battery_v_x10);
                self.push(&[hi, lo, hi.wrapping_add(lo)]); // CALORIES hijack: voltage x10
            }
            0x25 => self.push(&[0, 0, 0, 0, 0]),
            0x31 => {
                let v = if self.moving() { 0x31 } else { 0x30 };
                self.push(&[v, v]); // MOVING
            }
            _ => {}
        }
    }

    fn push(&mut self, bytes: &[u8]) {
        self.tx.extend(bytes.iter().copied());
    }
}

fn split(v: u16) -> (u8, u8) {
    ((v >> 8) as u8, v as u8)
}

fn write_len(op: u8) -> usize {
    match op {
        0x0B | 0x0C => 4, // PAS / MODE
        0x1A => 3,        // LIGHTS
        0x1F => 5,        // SPEED_LIM
        _ => 0,
    }
}

// ---- backend selection + serial bridge (used by serial.rs) -----------------

pub fn activate() {
    ACTIVE.store(true, Ordering::SeqCst);
    // Optional initial state for demos / headless tests.
    if let Ok(mut m) = MOTOR.lock() {
        if let Some(kph) = env_f32("EMU_MOTOR_SPEED") {
            m.speed_kph_x10 = (kph.clamp(0.0, 99.0) * 10.0) as u16;
        }
        if let Some(v) = env_f32("EMU_MOTOR_BATTERY") {
            m.battery_v_x10 = (v.clamp(0.0, 60.0) * 10.0) as u16;
        }
    }
}

fn env_f32(key: &str) -> Option<f32> {
    std::env::var(key).ok().and_then(|s| s.parse().ok())
}
pub fn is_active() -> bool {
    ACTIVE.load(Ordering::SeqCst)
}
pub fn write(bytes: &[u8]) {
    if let Ok(mut m) = MOTOR.lock() {
        m.feed(bytes);
    }
}
pub fn read_byte() -> Option<u8> {
    MOTOR.lock().ok().and_then(|mut m| m.tx.pop_front())
}

// ---- UI view + controls ----------------------------------------------------

pub struct View {
    pub pas: Option<u8>, // level 0..9, or None for push/walk
    pub push: bool,
    pub lights: bool,
    pub speed_kph_x10: u16,
    pub battery_v_x10: u16,
    pub moving: bool,
    pub temp_c: u8,
    pub amp_x2: u8,
    pub battery_pct: u8,
}

pub fn view() -> View {
    let m = MOTOR.lock().unwrap();
    View {
        pas: pas_level(m.pas_wire),
        push: m.pas_wire == 0x06,
        lights: m.lights,
        speed_kph_x10: m.speed_kph_x10,
        battery_v_x10: m.battery_v_x10,
        moving: m.moving(),
        temp_c: m.motor_temp_c,
        amp_x2: m.amp_x2(),
        battery_pct: m.battery_pct(),
    }
}

fn pas_level(wire: u8) -> Option<u8> {
    // Bafang's non-monotonic PAS wire encoding -> level number.
    Some(match wire {
        0x00 => 0,
        0x01 => 1,
        0x0B => 2,
        0x0C => 3,
        0x0D => 4,
        0x02 => 5,
        0x15 => 6,
        0x16 => 7,
        0x17 => 8,
        0x03 => 9,
        _ => return None, // 0x06 = push/walk, or unset
    })
}

pub fn speed_up() {
    if let Ok(mut m) = MOTOR.lock() {
        m.speed_kph_x10 = (m.speed_kph_x10 + 10).min(990); // +1 km/h, cap 99
    }
}
pub fn speed_down() {
    if let Ok(mut m) = MOTOR.lock() {
        m.speed_kph_x10 = m.speed_kph_x10.saturating_sub(10);
    }
}
pub fn batt_up() {
    if let Ok(mut m) = MOTOR.lock() {
        m.battery_v_x10 = (m.battery_v_x10 + 1).min(600); // +0.1 V, cap 60
    }
}
pub fn batt_down() {
    if let Ok(mut m) = MOTOR.lock() {
        m.battery_v_x10 = m.battery_v_x10.saturating_sub(1);
    }
}
