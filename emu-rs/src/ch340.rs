//! Userspace CH340/CH341 USB-serial driver (Route B).
//!
//! Lets the emulator drive a CH340 UART adapter (VID:PID `1a86:7523`) without
//! the kernel `ch341-uart` driver or a `/dev/ttyUSB*` node — the whole serial
//! protocol runs in userspace over libusb. The point is **Android/Termux**:
//! `termux-usb` hands the process an already-open USB file descriptor (via the
//! Android USB host API, no root), and `open_fd()` wraps it. On desktop Linux
//! `open_first()` claims the adapter by VID:PID (detaching the kernel driver)
//! for testing the same code path.
//!
//! Register/baud sequence follows the Linux kernel `ch341.c` driver.
//!
//! Built only with `--features usb` (pulls in `rusb`/libusb). Without it, the
//! stubs below report "not built" so the CLI can print a helpful message.

/// True when compiled with USB support (`--features usb`).
pub const AVAILABLE: bool = cfg!(feature = "usb");

#[cfg(feature = "usb")]
mod imp {
    use std::collections::VecDeque;
    use std::sync::Mutex;
    use std::time::Duration;

    use rusb::{Context, DeviceHandle, UsbContext};

    const VID: u16 = 0x1a86;
    const PID: u16 = 0x7523;
    // CH340 data endpoints (standard for this chip): bulk OUT 0x02, bulk IN 0x82.
    const EP_OUT: u8 = 0x02;
    const EP_IN: u8 = 0x82;

    // CH341 vendor requests.
    const REQ_READ_VERSION: u8 = 0x5f;
    const REQ_WRITE_REG: u8 = 0x9a;
    const REQ_SERIAL_INIT: u8 = 0xa1;
    const REQ_MODEM_CTRL: u8 = 0xa4;
    // Registers (paired in one write: high byte reg, low byte reg).
    const REG_DIVISOR: u16 = 0x13;
    const REG_PRESCALER: u16 = 0x12;
    const REG_LCR: u16 = 0x18;
    const REG_LCR2: u16 = 0x25;
    // LCR bits for 8N1.
    const LCR_ENABLE_RX: u16 = 0x80;
    const LCR_ENABLE_TX: u16 = 0x40;
    const LCR_CS8: u16 = 0x03;
    // Modem control: assert DTR (bit5) + RTS (bit6).
    const MCR_DTR_RTS: u8 = 0x60;

    const CTL_TIMEOUT: Duration = Duration::from_millis(200);

    struct Ch340 {
        handle: DeviceHandle<Context>,
        rx: VecDeque<u8>,
    }

    static CH340: Mutex<Option<Ch340>> = Mutex::new(None);

    /// Wrap an already-open USB fd (Android `termux-usb`).
    pub fn open_fd(fd: i32, baud: u32) -> Result<(), String> {
        // Android sandboxes /dev/bus/usb, so libusb's device scan inside
        // libusb_init() fails ("Input/Output Error"). Disable discovery before
        // creating the context — we don't need it, we just wrap the fd that
        // termux-usb already opened for us. Must be set before Context::new().
        rusb::disable_device_discovery()
            .map_err(|e| format!("libusb disable-discovery: {e}"))?;
        let ctx = Context::new().map_err(|e| format!("libusb init: {e}"))?;
        // SAFETY: `fd` must be an open USB device fd owned by us (termux-usb
        // hands it over for the lifetime of the process). libusb takes over I/O
        // but does not close it.
        let handle = unsafe { ctx.open_device_with_fd(fd) }
            .map_err(|e| format!("open fd {fd}: {e}"))?;
        init(handle, baud)
    }

    /// Claim the first CH340 (`1a86:7523`) by VID:PID — desktop Linux testing.
    pub fn open_first(baud: u32) -> Result<(), String> {
        let ctx = Context::new().map_err(|e| format!("libusb init: {e}"))?;
        // Find the device first so we can tell "absent" from "no permission".
        let dev = ctx
            .devices()
            .map_err(|e| format!("list usb devices: {e}"))?
            .iter()
            .find(|d| {
                d.device_descriptor()
                    .map(|dd| dd.vendor_id() == VID && dd.product_id() == PID)
                    .unwrap_or(false)
            })
            .ok_or_else(|| format!("no CH340 ({VID:04x}:{PID:04x}) plugged in"))?;
        let handle = dev.open().map_err(|e| match e {
            rusb::Error::Access => format!(
                "CH340 found but access denied — the usbfs node is root-only. \
                 Run as root, or add a udev rule for {VID:04x}:{PID:04x}. \
                 (On Android this doesn't apply: use --motor-fd with termux-usb.)"
            ),
            other => format!("open CH340: {other}"),
        })?;
        init(handle, baud)
    }

    fn init(handle: DeviceHandle<Context>, baud: u32) -> Result<(), String> {
        // Take the port from the kernel's ch341-uart driver if it's bound.
        let _ = handle.set_auto_detach_kernel_driver(true);
        handle
            .claim_interface(0)
            .map_err(|e| format!("claim interface 0: {e}"))?;
        configure(&handle, baud)?;
        *CH340.lock().unwrap() = Some(Ch340 {
            handle,
            rx: VecDeque::new(),
        });
        Ok(())
    }

    fn ctl_out(h: &DeviceHandle<Context>, req: u8, val: u16, idx: u16) -> Result<(), String> {
        h.write_control(0x40, req, val, idx, &[], CTL_TIMEOUT)
            .map(|_| ())
            .map_err(|e| format!("control out req={req:#04x}: {e}"))
    }

    fn configure(h: &DeviceHandle<Context>, baud: u32) -> Result<(), String> {
        // Read (and ignore) the chip version, as the kernel does.
        let mut ver = [0u8; 2];
        let _ = h.read_control(0xc0, REQ_READ_VERSION, 0, 0, &mut ver, CTL_TIMEOUT);
        ctl_out(h, REQ_SERIAL_INIT, 0, 0)?;
        // Baud: write prescaler+divisor pair, bit7 flushes each byte immediately
        // (don't wait to fill a 32-byte USB packet — matters at 1200 baud).
        let div = divisor(baud)? | 0x80;
        ctl_out(h, REQ_WRITE_REG, (REG_DIVISOR << 8) | REG_PRESCALER, div)?;
        // Line control: 8 data bits, no parity, 1 stop; TX+RX enabled.
        let lcr = LCR_ENABLE_RX | LCR_ENABLE_TX | LCR_CS8;
        ctl_out(h, REQ_WRITE_REG, (REG_LCR2 << 8) | REG_LCR, lcr)?;
        // Modem control (bRequest expects inverted bits).
        ctl_out(h, REQ_MODEM_CTRL, !(MCR_DTR_RTS as u16) & 0xff, 0)?;
        Ok(())
    }

    // Port of ch341_get_divisor() from the Linux kernel: pick prescaler `ps`,
    // base-clock factor `fact`, and divisor for the requested baud. Returns the
    // 16-bit value written to the divisor/prescaler register pair.
    fn divisor(speed: u32) -> Result<u16, String> {
        const CLK: u32 = 48_000_000;
        let clk_div = |ps: i32, fact: u32| 1u32 << (12 - 3 * ps as u32 - fact);
        let min_rate = |ps: i32| CLK / (clk_div(ps, 1) * 512);

        let speed = speed.clamp(46, 3_000_000);
        let mut fact = 1u32;
        let mut ps = 3i32;
        while ps >= 0 && speed <= min_rate(ps) {
            ps -= 1;
        }
        if ps < 0 {
            return Err("baud rate too low for CH340".into());
        }
        let mut cd = clk_div(ps, fact);
        let mut div = CLK / (cd * speed);
        if div == 0 {
            return Err("baud rate too high for CH340".into());
        }
        // Round to nearest.
        if (CLK / (cd * div)) - speed < speed - (CLK / (cd * (div + 1))) {
            div += 1;
        }
        if div < 9 || div > 255 {
            div /= 2;
            cd /= 2;
            fact = 0;
        }
        let _ = cd;
        if div < 2 {
            return Err("baud rate unsupported by CH340".into());
        }
        // Prefer the lower base clock when the divisor is even.
        if fact == 1 && div % 2 == 0 {
            div /= 2;
            fact = 0;
        }
        Ok((((0x100 - div) as u16) << 8) | ((fact as u16) << 2) | (ps as u16))
    }

    pub fn is_active() -> bool {
        CH340.lock().map(|g| g.is_some()).unwrap_or(false)
    }

    pub fn write(bytes: &[u8]) {
        if let Ok(mut g) = CH340.lock() {
            if let Some(c) = g.as_mut() {
                let _ = c.handle.write_bulk(EP_OUT, bytes, Duration::from_millis(100));
            }
        }
    }

    pub fn read_byte() -> Option<u8> {
        let mut g = CH340.lock().ok()?;
        let c = g.as_mut()?;
        if c.rx.is_empty() {
            // Pull a chunk; a short timeout keeps the tick loop responsive when
            // the motor is quiet (rusb returns Timeout, which we treat as idle).
            let mut buf = [0u8; 64];
            if let Ok(n) = c.handle.read_bulk(EP_IN, &mut buf, Duration::from_millis(5)) {
                c.rx.extend(&buf[..n]);
            }
        }
        c.rx.pop_front()
    }
}

#[cfg(feature = "usb")]
pub use imp::{is_active, open_fd, open_first, read_byte, write};

// ---- stubs when built without --features usb -------------------------------

#[cfg(not(feature = "usb"))]
mod stub {
    const MSG: &str = "USB support not built — rebuild with `--features usb`";

    pub fn open_fd(_fd: i32, _baud: u32) -> Result<(), String> {
        Err(MSG.into())
    }
    pub fn open_first(_baud: u32) -> Result<(), String> {
        Err(MSG.into())
    }
    pub fn is_active() -> bool {
        false
    }
    pub fn write(_bytes: &[u8]) {}
    pub fn read_byte() -> Option<u8> {
        None
    }
}

#[cfg(not(feature = "usb"))]
pub use stub::{is_active, open_fd, open_first, read_byte, write};
