{
  description = "swang-stodva — open-source Bafang display firmware for the SW102";

  # Usage:
  #   nix run   .#emu        build & run the terminal emulator (Rust + ratatui)
  #   nix build .#emu        emulator binary  -> result/bin/swang-stodva-emu
  #   nix build .#firmware   on-target image  -> result/nrf51822_sw102.hex (+ .map)
  #   nix develop            dev shell (rustc/cargo + arm-none-eabi toolchain + python3)
  # See docs/BUILD.md for flashing, the OTA/release flow, and the motor mock.

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        # Cosmetic package label only; the release version of record lives in
        # version.mk (VERSION_STRING), managed by the release workflow.
        version = "0.0.1-alpha";

        # Terminal (ratatui) emulator — the Rust crate in emu-rs/, whose build.rs
        # compiles the firmware C via the cc crate. src is the whole repo so
        # build.rs can reach ../src, ../include, ../assets.
        emu = pkgs.rustPlatform.buildRustPackage {
          pname = "swang-stodva-emu";
          inherit version;
          src = ./.;
          cargoRoot = "emu-rs"; # Cargo.lock / vendoring live in the subdir
          buildAndTestSubdir = "emu-rs";
          cargoLock.lockFile = ./emu-rs/Cargo.lock;
          doCheck = false; # no tests; the binary is an interactive TUI
          meta.mainProgram = "swang-stodva-emu";
        };

        # On-target firmware for the nRF51x22 (Cortex-M0) -> signed-flashable .hex.
        firmware = pkgs.stdenv.mkDerivation {
          pname = "swang-stodva-firmware";
          inherit version;
          src = ./.;
          nativeBuildInputs = [ pkgs.gcc-arm-embedded pkgs.gnumake ];
          dontFixup = true; # output is Intel HEX + map, no host ELF to fix up
          enableParallelBuilding = true;
          buildPhase = ''
            runHook preBuild
            export GNU_INSTALL_ROOT=${pkgs.gcc-arm-embedded}
            make GNU_INSTALL_ROOT=$GNU_INSTALL_ROOT _build/nrf51822_sw102.hex
            runHook postBuild
          '';
          installPhase = ''
            runHook preInstall
            install -Dm644 _build/nrf51822_sw102.hex $out/nrf51822_sw102.hex
            install -Dm644 _build/nrf51822_sw102.map $out/nrf51822_sw102.map || true
            runHook postInstall
          '';
        };
      in {
        # Reuse the classic shell.nix so `nix develop` and `nix-shell` match.
        devShells.default = import ./shell.nix { inherit pkgs; };

        packages = {
          inherit emu firmware;
          default = emu;
        };

        apps.emu = flake-utils.lib.mkApp { drv = emu; };
      });
}
