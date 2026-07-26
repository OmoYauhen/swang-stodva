//! swang-stodva-emu — terminal (ratatui) emulator for the SW102 firmware.
//!
//! Compiles the real firmware C (UI, screens, state machine, Bafang protocol)
//! via build.rs and drives it: a 20 ms tick loop calls the firmware's
//! `emu_gui_tick()` + `ui_update()`, the 64×128 OLED framebuffer is rendered to
//! the terminal with Unicode braille, key presses map to the four buttons, and
//! the firmware's motor UART is served either by the built-in BBSHD emulation
//! (motor.rs, default) or a real pty/serial port (serial.rs, --motor-port).

mod ch340;
mod motor;
mod serial;

use std::io::{self, Stdout};
use std::os::raw::{c_int, c_void};
use std::time::{Duration, Instant};

use crossterm::{
    event::{self, Event, KeyCode, KeyEventKind, KeyModifiers},
    execute,
    terminal::{disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen},
};
use ratatui::{
    backend::CrosstermBackend,
    layout::Rect,
    style::{Color, Style},
    text::{Line, Span},
    widgets::{Block, Borders, Paragraph},
    Terminal,
};

extern "C" {
    fn eeprom_init();
    fn ui_update();
    fn emu_gui_tick();
    fn emu_set_button(idx: c_int, pressed: bool);
    fn showScreen(s: *const c_void);
    fn emu_framebuffer() -> *const u8;
    fn emu_should_quit() -> c_int;
    static screen_boot: u8; // opaque; only its address is needed
}

const W: usize = 64; // OLED width  (x)
const H: usize = 128; // OLED height (y)

// Buttons, in emu_set_button() index order.
const UP: usize = 0;
const DOWN: usize = 1;
const ENTER: usize = 2; // M / menu
const ESC: usize = 3; // P / power

// How the emulator is sourcing motor data.
struct MotorInfo {
    connected: bool,
    label: String,   // shown on the display frame's bottom border
    builtin: bool,    // true => built-in BBSHD (show + drive the motor panel)
}

// A press holds the button at least this long; OS key-repeat while held keeps
// extending it (→ the firmware sees a long-press), and a real key-release event
// (kitty et al.) clears it early. Long enough to bridge the auto-repeat initial
// delay so a genuine hold isn't misread as a tap, short enough that a tap stays
// well under the firmware's ~1 s long-press threshold.
const HOLD: Duration = Duration::from_millis(600);
const TICK: Duration = Duration::from_millis(20);

#[inline]
fn pixel(fb: *const u8, x: usize, y: usize) -> bool {
    // framebuffer.u8[x*(128/8) + (y/8)] bit (y&7)
    unsafe { (*fb.add(x * (H / 8) + (y >> 3)) >> (y & 7)) & 1 != 0 }
}

// Pack the 64×128 mono framebuffer into Unicode braille (2×4 px per cell) →
// 32×32 character cells, keeping the display's 1:2 aspect while fitting a
// normal terminal.
fn braille_lines(fb: *const u8) -> Vec<String> {
    const CW: usize = 2; // px per cell, x
    const CH: usize = 4; // px per cell, y
    // Braille dot bit per (dx, dy): left column 1,2,3,7 / right column 4,5,6,8.
    const DOT: [[u8; CH]; CW] = [[0x01, 0x02, 0x04, 0x40], [0x08, 0x10, 0x20, 0x80]];
    let mut out = Vec::with_capacity(H / CH);
    for cy in 0..(H / CH) {
        let mut s = String::with_capacity(W / CW);
        for cx in 0..(W / CW) {
            let mut bits: u8 = 0;
            for dx in 0..CW {
                for dy in 0..CH {
                    if pixel(fb, cx * CW + dx, cy * CH + dy) {
                        bits |= DOT[dx][dy];
                    }
                }
            }
            s.push(char::from_u32(0x2800 + bits as u32).unwrap_or(' '));
        }
        out.push(s);
    }
    out
}

fn render(
    term: &mut Terminal<CrosstermBackend<Stdout>>,
    info: &MotorInfo,
    pressed: &[bool; 4],
    motor_pressed: &[bool; 4],
) -> io::Result<()> {
    let fb = unsafe { emu_framebuffer() };

    let lines: Vec<Line> = braille_lines(fb)
        .into_iter()
        .map(|s| Line::from(Span::styled(s, Style::default().fg(Color::White))))
        .collect();

    // Fixed frame: 32×32 braille cells + 1-cell border on each side.
    let fw = (W / 2 + 2) as u16;
    let fh = (H / 4 + 2) as u16;

    // Motor status (dot + text) shown on the frame's bottom border.
    let dot = if info.connected { Color::Green } else { Color::Red };
    let text = info.label.clone();
    let motor_line = Line::from(vec![
        Span::raw(" "),
        Span::styled("\u{25CF}", Style::default().fg(dot)), // ●
        Span::raw(" "),
        Span::styled(text, Style::default().fg(Color::Gray)),
        Span::raw(" "),
    ])
    .centered();

    // Line 2: button chips, centered within the frame width.
    let labels = ["\u{2191}", "\u{2193}", "ent", "esc"]; // ↑ ↓ ent esc
    let total: usize =
        labels.iter().map(|l| l.chars().count() + 2).sum::<usize>() + (labels.len() - 1);
    let pad = (fw as usize).saturating_sub(total) / 2;
    let mut btn: Vec<Span> = vec![Span::raw(" ".repeat(pad))];
    for (i, l) in labels.iter().enumerate() {
        let st = if pressed[i] {
            Style::default().fg(Color::Black).bg(Color::White)
        } else {
            Style::default().fg(Color::Gray)
        };
        btn.push(Span::styled(format!("[{l}]"), st));
        if i + 1 < labels.len() {
            btn.push(Span::raw(" "));
        }
    }
    let buttons_line = Line::from(btn);

    // Built-in motor panel (rendered to the right of the display frame).
    let panel: Option<Vec<Line>> = if info.builtin {
        Some(motor_panel(&motor::view(), motor_pressed))
    } else {
        None
    };

    term.draw(|f| {
        let area = f.area();
        // Static size anchored top-left; never stretched to the terminal.
        let frame = Rect {
            x: 0,
            y: 0,
            width: fw.min(area.width),
            height: fh.min(area.height),
        };
        let block = Block::default()
            .borders(Borders::ALL)
            .title(
                Line::from(Span::styled(
                    " SW102 display emulator ",
                    Style::default().fg(Color::Yellow),
                ))
                .centered(),
            )
            .title_bottom(motor_line);
        f.render_widget(Paragraph::new(lines).block(block), frame);

        if area.height > fh {
            f.render_widget(
                Paragraph::new(buttons_line),
                Rect { x: 0, y: fh, width: area.width, height: 1 },
            );
        }

        if let Some(plines) = panel {
            let pw = 24u16;
            let ph = (plines.len() + 2) as u16;
            let px = fw + 1; // one-column gap after the display frame
            if area.width > px {
                let prect = Rect {
                    x: px,
                    y: 0,
                    width: pw.min(area.width - px),
                    height: ph.min(area.height),
                };
                let pblock = Block::default().borders(Borders::ALL).title(
                    Line::from(Span::styled(" BBSHD motor emulator ", Style::default().fg(Color::Cyan)))
                        .centered(),
                );
                f.render_widget(Paragraph::new(plines).block(pblock), prect);
            }
        }
    })?;
    Ok(())
}

// Lines for the built-in-motor panel. `pressed` = [speed-, speed+, batt-, batt+]
// control keys, highlighted like the display's button chips while held.
fn motor_panel(v: &motor::View, pressed: &[bool; 4]) -> Vec<Line<'static>> {
    let gray = Style::default().fg(Color::Gray);
    let white = Style::default().fg(Color::White);
    let chip = |on: bool| {
        if on {
            Style::default().fg(Color::Black).bg(Color::White)
        } else {
            gray
        }
    };
    // "PAS:" style row: " {label:<13} value" — value aligns at column 15.
    let row = |label: &str, val: String| {
        Line::from(vec![
            Span::styled(format!(" {label:<13} "), gray),
            Span::styled(val, white),
        ])
    };
    // Control row: label + two highlightable chips, same 15-column value offset.
    let ctrl = |label: &str, a: &'static str, b: &'static str, pa: bool, pb: bool, val: String| {
        Line::from(vec![
            Span::styled(format!(" {label} "), gray),
            Span::styled(a, chip(pa)),
            Span::styled(b, chip(pb)),
            Span::styled(": ", gray),
            Span::styled(val, white),
        ])
    };

    let pas = if v.push {
        "push".to_string()
    } else {
        v.pas.map_or_else(|| "-".to_string(), |n| n.to_string())
    };
    vec![
        row("PAS:", pas),
        row("Light:", if v.lights { "on".into() } else { "off".into() }),
        ctrl("Speed", "[\u{2190}]", "[\u{2192}]", pressed[0], pressed[1],
             format!("{} km/h", v.speed_kph_x10 / 10)),
        ctrl("Bat  ", "[-]", "[+]", pressed[2], pressed[3],
             format!("{}.{} V", v.battery_v_x10 / 10, v.battery_v_x10 % 10)),
        Line::from(""),
        row("Moving:", if v.moving { "yes".into() } else { "no".into() }),
        row("Temp:", format!("{} \u{b0}C", v.temp_c)),
        row("Current:", format!("{}.{} A", v.amp_x2 / 2, (v.amp_x2 % 2) * 5)),
        row("Battery:", format!("{} %", v.battery_pct)),
    ]
}

fn map_key(code: KeyCode) -> Option<usize> {
    match code {
        KeyCode::Up => Some(UP),
        KeyCode::Down => Some(DOWN),
        KeyCode::Enter | KeyCode::Char('m') | KeyCode::Char('M') => Some(ENTER),
        KeyCode::Esc | KeyCode::Char('p') | KeyCode::Char('P') => Some(ESC),
        _ => None,
    }
}

// Built-in-motor control keys -> chip index: 0 speed- 1 speed+ 2 batt- 3 batt+.
fn motor_key(code: KeyCode) -> Option<usize> {
    match code {
        KeyCode::Left | KeyCode::Char('<') | KeyCode::Char(',') => Some(0),
        KeyCode::Right | KeyCode::Char('>') | KeyCode::Char('.') => Some(1),
        KeyCode::Char('-') | KeyCode::Char('_') => Some(2),
        KeyCode::Char('+') | KeyCode::Char('=') => Some(3),
        _ => None,
    }
}

fn run(term: &mut Terminal<CrosstermBackend<Stdout>>, info: &MotorInfo) -> io::Result<()> {
    // A press extends the button's deadline; a real key-release clears it early.
    // pressed = deadline still in the future. Applies to both the four firmware
    // buttons and the four built-in-motor control keys.
    let mut btn_deadline: [Option<Instant>; 4] = [None; 4];
    let mut motor_deadline: [Option<Instant>; 4] = [None; 4];
    let mut pressed = [false; 4];
    let mut applied = [false; 4];
    let mut motor_pressed = [false; 4];
    let mut next_tick = Instant::now();

    loop {
        // ---- input ----
        while event::poll(Duration::from_millis(0))? {
            if let Event::Key(k) = event::read()? {
                // Ctrl-C quits.
                if k.modifiers.contains(KeyModifiers::CONTROL) && k.code == KeyCode::Char('c') {
                    return Ok(());
                }
                let idx = map_key(k.code);
                let midx = if info.builtin { motor_key(k.code) } else { None };
                match k.kind {
                    KeyEventKind::Release => {
                        if let Some(i) = idx {
                            btn_deadline[i] = None;
                        }
                        if let Some(m) = midx {
                            motor_deadline[m] = None;
                        }
                    }
                    _ => {
                        // Press or Repeat
                        let dl = Instant::now() + HOLD;
                        if let Some(i) = idx {
                            btn_deadline[i] = Some(dl);
                        } else if let Some(m) = midx {
                            motor_deadline[m] = Some(dl);
                            match m {
                                0 => motor::speed_down(),
                                1 => motor::speed_up(),
                                2 => motor::batt_down(),
                                _ => motor::batt_up(),
                            }
                        }
                    }
                }
            }
        }

        // ---- apply button state ----
        let now = Instant::now();
        for i in 0..4 {
            pressed[i] = btn_deadline[i].map_or(false, |d| now < d);
            motor_pressed[i] = motor_deadline[i].map_or(false, |d| now < d);
            if pressed[i] != applied[i] {
                applied[i] = pressed[i];
                unsafe { emu_set_button(i as c_int, pressed[i]) };
            }
        }

        // ---- firmware ticks (catch up to real time) ----
        while now >= next_tick {
            unsafe {
                emu_gui_tick();
                ui_update();
            }
            next_tick += TICK;
        }

        if unsafe { emu_should_quit() } != 0 {
            return Ok(());
        }

        render(term, info, &pressed, &motor_pressed)?;
        std::thread::sleep(Duration::from_millis(5));
    }
}

/// Headless smoke run (EMU_HEADLESS=1): no terminal — tick the firmware for
/// EMU_HEADLESS_MS (default 4000) ms, then print the framebuffer as ASCII.
/// Lets the emulator be verified without a TTY (CI / dev).
fn run_headless() {
    let ms: u64 = std::env::var("EMU_HEADLESS_MS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(4000);
    let start = Instant::now();
    let mut next = start;
    while start.elapsed() < Duration::from_millis(ms) {
        let now = Instant::now();
        while now >= next {
            unsafe {
                emu_gui_tick();
                ui_update();
            }
            next += TICK;
        }
        if unsafe { emu_should_quit() } != 0 {
            break;
        }
        std::thread::sleep(Duration::from_millis(2));
    }
    let fb = unsafe { emu_framebuffer() };
    for s in braille_lines(fb) {
        println!("{s}");
    }
}

// How the emulator was told to source motor data.
enum Backend {
    BuiltIn,      // no flag: in-process BBSHD
    Port(String), // --motor-port=PATH: kernel pty/serial device
    UsbFd(i32),   // --motor-fd=N: userspace CH340 over a termux-usb fd (Android)
    UsbFirst,     // --motor-usb: userspace CH340 by VID:PID (desktop testing)
}

fn parse_args() -> Backend {
    let mut backend = Backend::BuiltIn;
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        if let Some(v) = a.strip_prefix("--motor-port=") {
            backend = Backend::Port(v.to_string());
        } else if a == "--motor-port" {
            backend = args.next().map(Backend::Port).unwrap_or(Backend::BuiltIn);
        } else if let Some(v) = a.strip_prefix("--motor-fd=") {
            match v.parse() {
                Ok(fd) => backend = Backend::UsbFd(fd),
                Err(_) => {
                    eprintln!("swang-stodva-emu: --motor-fd needs a number, got '{v}'");
                    std::process::exit(2);
                }
            }
        } else if a == "--motor-usb" {
            backend = Backend::UsbFirst;
        } else if a == "-h" || a == "--help" {
            print_help();
            std::process::exit(0);
        } else {
            eprintln!("swang-stodva-emu: unknown argument '{a}' (try --help)");
            std::process::exit(2);
        }
    }
    backend
}

fn print_help() {
    let usb = if ch340::AVAILABLE { "" } else { " (needs --features usb)" };
    println!(
        "swang-stodva-emu — SW102 terminal emulator\n\n\
         Usage: swang-stodva-emu [MOTOR]\n\n\
         Motor source (default: built-in BBSHD emulation):\n  \
         --motor-port=PATH   UART device: pty or kernel serial (/dev/ttyUSB0)\n  \
         --motor-fd=N        userspace CH340 over an open USB fd{usb};\n                      \
             for Android/termux-usb (no root, no /dev/ttyUSB)\n  \
         --motor-usb         userspace CH340 by VID:PID 1a86:7523{usb}\n\n\
         Other:\n  \
         -h, --help          show this help"
    );
}

// Turn a ch340::open_* result into a MotorInfo, warning (not exiting) on error
// so the emulator still starts and shows the display, just without motor data.
fn usb_motor(res: Result<(), String>, label: String) -> MotorInfo {
    match res {
        Ok(()) => {
            eprintln!("swang-stodva-emu: {label} opened @1200 baud");
            MotorInfo { connected: true, label, builtin: false }
        }
        Err(e) => {
            eprintln!("swang-stodva-emu: {e}; running blind");
            MotorInfo { connected: false, label: "motor not connected".into(), builtin: false }
        }
    }
}

fn main() -> io::Result<()> {
    let info = match parse_args() {
        Backend::Port(p) => {
            if serial::init(&p) {
                MotorInfo { connected: true, label: format!("connected to {p}"), builtin: false }
            } else {
                eprintln!("swang-stodva-emu: failed to open motor port '{p}'; running blind");
                MotorInfo { connected: false, label: "motor not connected".into(), builtin: false }
            }
        }
        Backend::UsbFd(fd) => usb_motor(ch340::open_fd(fd, 1200), format!("CH340 (fd {fd})")),
        Backend::UsbFirst => usb_motor(ch340::open_first(1200), "CH340 (usb)".into()),
        Backend::BuiltIn => {
            motor::activate();
            MotorInfo { connected: true, label: "built-in BBSHD".into(), builtin: true }
        }
    };

    unsafe {
        eeprom_init();
        showScreen(core::ptr::addr_of!(screen_boot) as *const c_void);
    }

    if std::env::var("EMU_HEADLESS").is_ok() {
        run_headless();
        return Ok(());
    }

    enable_raw_mode()?;
    let mut stdout = io::stdout();
    execute!(stdout, EnterAlternateScreen, crossterm::cursor::Hide)?;
    // Best-effort: ask for key-release events on terminals that support it.
    let _ = execute!(
        stdout,
        event::PushKeyboardEnhancementFlags(event::KeyboardEnhancementFlags::REPORT_EVENT_TYPES)
    );
    let mut term = Terminal::new(CrosstermBackend::new(stdout))?;
    term.clear()?;

    let res = run(&mut term, &info);

    let _ = execute!(term.backend_mut(), event::PopKeyboardEnhancementFlags);
    disable_raw_mode()?;
    execute!(term.backend_mut(), LeaveAlternateScreen, crossterm::cursor::Show)?;
    res
}
