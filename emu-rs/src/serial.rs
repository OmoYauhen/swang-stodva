//! Motor UART over a real pty/serial port (e.g. `--motor-port=/dev/ttyUSB0`).
//! Exposes byte I/O to the C UART shim (csrc/uart.c) via `emu_serial_write` /
//! `emu_serial_read_byte`. Those hooks pick a backend: the built-in BBSHD model
//! (`crate::motor`) or this pty/serial FD.

use crate::motor;
use std::ffi::CString;
use std::sync::atomic::{AtomicI32, Ordering};

static FD: AtomicI32 = AtomicI32::new(-1);

extern "C" {
    // defined in ../src/emu/adc.c; the firmware's battery-voltage source
    static mut emu_voltage: u16;
}

/// Open the given motor port. Returns true on success.
pub fn init(path: &str) -> bool {
    let c = match CString::new(path) {
        Ok(c) => c,
        Err(_) => return false,
    };
    let fd = unsafe { libc::open(c.as_ptr(), libc::O_RDWR | libc::O_NOCTTY | libc::O_NONBLOCK) };
    if fd < 0 {
        return false;
    }
    unsafe { configure(fd) };
    FD.store(fd, Ordering::SeqCst);
    // The firmware defaults battery voltage to 50 (5.0 V) via adc.c; a real
    // motor connection bumps it to a sane 48 V so the UI isn't nonsense.
    unsafe { emu_voltage = 480 };
    true
}

unsafe fn configure(fd: i32) {
    // Raw + 1200 baud. Cosmetic on a pty, but a real USB UART needs it.
    let mut t: libc::termios = std::mem::zeroed();
    if libc::tcgetattr(fd, &mut t) == 0 {
        libc::cfmakeraw(&mut t);
        let _ = libc::cfsetispeed(&mut t, libc::B1200);
        let _ = libc::cfsetospeed(&mut t, libc::B1200);
        libc::tcsetattr(fd, libc::TCSANOW, &t);
    }
    // Discard anything buffered on the port from a previous emu session. The
    // Bafang framing has no start byte or CRC, so a single leftover reply byte
    // would offset every subsequent read and desync RX permanently (speed etc.
    // stop updating until the motor/mock is restarted).
    libc::tcflush(fd, libc::TCIOFLUSH);
}

#[no_mangle]
pub extern "C" fn emu_serial_write(buf: *const u8, len: i32) {
    if buf.is_null() || len <= 0 {
        return;
    }
    if motor::is_active() {
        let slice = unsafe { std::slice::from_raw_parts(buf, len as usize) };
        motor::write(slice);
        return;
    }
    let fd = FD.load(Ordering::SeqCst);
    if fd >= 0 {
        unsafe {
            let _ = libc::write(fd, buf as *const libc::c_void, len as usize);
        }
    }
}

#[no_mangle]
pub extern "C" fn emu_serial_read_byte(out: *mut u8) -> i32 {
    if out.is_null() {
        return 0;
    }
    if motor::is_active() {
        return match motor::read_byte() {
            Some(b) => {
                unsafe { *out = b };
                1
            }
            None => 0,
        };
    }
    let fd = FD.load(Ordering::SeqCst);
    if fd < 0 {
        return 0;
    }
    let mut b: u8 = 0;
    let n = unsafe { libc::read(fd, &mut b as *mut u8 as *mut libc::c_void, 1) };
    if n == 1 {
        unsafe { *out = b };
        1
    } else {
        0
    }
}
