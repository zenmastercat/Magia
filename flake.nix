# SPDX-License-Identifier: BSD-2-Clause
{
  description = "Magia: a from-scratch x86_64 teaching kernel mixing FreeBSD and Linux ideas";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      # `nix develop -c make test` uses the same shell as `nix-shell`.
      devShells = forAllSystems (pkgs: {
        default = import ./shell.nix { inherit pkgs; };
      });
    };
}
