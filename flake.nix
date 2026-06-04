{
  description = "raft-l2-oracle - L2 failure detection oracle using Raft on STM32";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in
      {
        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            # STM32 cross-compiler
            gcc-arm-embedded

            # Flash + debug
            openocd
            stlink

            # Build system
            cmake
            ninja

            # Host builds (simulator)
            gcc
            gnumake

            # Testing + debugging
            valgrind
            gdb

            # Tools
            python3
            python3Packages.scapy

            # Analysis
            tcpdump
            wireshark-cli
          ];

          shellHook = ''
            echo "raft-l2-oracle dev environment"
            echo "  arm-none-eabi-gcc: $(arm-none-eabi-gcc --version 2>&1 | head -1)"
            echo "  cmake: $(cmake --version | head -1)"
            echo ""
            echo "Quick start:"
            echo "  cd sim && mkdir -p build && cd build && cmake .. && make"
          '';
        };
      }
    );
}
