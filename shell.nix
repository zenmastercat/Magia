# SPDX-License-Identifier: BSD-2-Clause
# Development shell for Magia: `nix-shell --run 'make test'`.
{ pkgs ? import <nixpkgs> { } }:
pkgs.mkShell {
  name = "magia-dev";
  packages = with pkgs; [
    gcc
    binutils
    gnumake
    gawk
    rustup
    qemu
    python3
  ];
  # The Nix cc-wrapper would otherwise inject userland hardening flags
  # (-fPIE, _FORTIFY_SOURCE, ...) that make no sense for a freestanding
  # kernel. Magia passes its own kernel hardening flags in the Makefile.
  hardeningDisable = [ "all" ];
  shellHook = ''
    export RUSTUP_TOOLCHAIN_AUTO_INSTALL=1
  '';
}
