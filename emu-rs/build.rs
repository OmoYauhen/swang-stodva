// Compile the SW102 firmware C sources plus the desktop HAL shims into a static
// library that the Rust emulator links against and drives over FFI.
//
// The firmware (state, eeprom, gfx, ui, screens) is compiled unchanged; the
// desktop HAL is provided by src/emu/{button,eeprom_hw,ble_services}.c and
// the csrc/{hal,uart}.c shims in this crate.

use std::path::Path;

fn main() {
    let root = Path::new("..");

    let firmware = [
        // shared / protocol / state
        "src/utils.c",
        "src/state.c",
        "src/eeprom.c",
        // SW102 UI + framework
        "src/gfx.c",
        "src/ui.c",
        "src/buttons.c",
        "src/screen_boot.c",
        "src/screen_main.c",
        "src/screen_cfg.c",
        "src/screen_cfg_utils.c",
        "src/screen_cfg_tree.c",
        // desktop HAL shims that are plain C already (reused verbatim)
        "src/emu/eeprom_hw.c",
        "src/emu/ble_services.c",
        "src/emu/button.c",
    ];

    let mut build = cc::Build::new();
    build
        .include(root.join("src"))
        .include(root.join("assets"))
        .define("BOARD_CUSTOM", None)
        .define("SW102", None)
        .define("VERSION_STRING", "\"emu\"")
        .flag("-fno-builtin")
        .flag("-fshort-enums") // firmware ABI expects short enums throughout
        .flag_if_supported("-Wno-unused-parameter")
        .flag_if_supported("-Wno-unused-but-set-variable")
        .flag_if_supported("-Wno-unused-variable")
        .warnings(false);

    for f in firmware {
        build.file(root.join(f));
    }
    // HAL shims specific to this crate
    build.file("csrc/hal.c");
    build.file("csrc/uart.c");

    build.compile("swang_firmware");

    println!("cargo:rerun-if-changed=csrc/hal.c");
    println!("cargo:rerun-if-changed=csrc/uart.c");
    println!("cargo:rerun-if-changed=build.rs");
    // rebuild when firmware C changes
    for f in firmware {
        println!("cargo:rerun-if-changed=../{f}");
    }
    // rebuild when any included header or asset (e.g. *.xbm icons) changes
    for dir in ["../src", "../assets"] {
        println!("cargo:rerun-if-changed={dir}");
        if let Ok(entries) = std::fs::read_dir(dir) {
            for e in entries.flatten() {
                println!("cargo:rerun-if-changed={}", e.path().display());
            }
        }
    }
}
