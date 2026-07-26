{ pkgs ? import <nixpkgs> {} }:

# Dev shell for both builds:
#   - terminal emulator  (cd emu-rs && cargo run), and
#   - on-target firmware (make _build/nrf51822_sw102.hex).
# On any machine that has Nix installed, from the repo root:
#
#     nix-shell          # or: nix develop   (flake.nix reuses this shell)
#     cd emu-rs && cargo run
#
# See docs/BUILD.md for full build/flash/OTA instructions and the mock motor.

pkgs.mkShell {
  buildInputs = with pkgs; [
    # terminal emulator (Rust crate emu-rs/; cc-crate compiles the firmware C)
    rustc
    cargo
    gcc
    python3
    # on-target firmware (nRF51 / Cortex-M0)
    gnumake
    gcc-arm-embedded
    srecord
  ];

  shellHook = ''
    # Point the SDK Makefile at the Nix arm-none-eabi toolchain.
    export GNU_INSTALL_ROOT="${pkgs.gcc-arm-embedded}"

    echo "swang-stodva dev shell ready."
    echo "  emu:      cd emu-rs && cargo run"
    echo "  firmware: make _build/nrf51822_sw102.hex"
    echo "  mock:     python3 -u tools/bbshd_mock.py --verbose --speed 18"
    echo "  mock (sweep speed to test the UI):"
    echo "            python3 -u tools/bbshd_mock.py --speed-wave sine --speed-min 0 --speed-max 45"
    echo "            then (in emu-rs/): cargo run -- --motor-port=/dev/pts/N"
  '';
}
