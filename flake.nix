{
  description = "penguin-qemu: upstream QEMU release + the IGLOO patch series";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs { inherit system; };
        bases = builtins.fromJSON (builtins.readFile ./base.json);

        # One live version, rebased forward. patches/<version>/ is keyed by
        # upstream release, so adding a second live version is additive -- but we
        # deliberately maintain exactly one.
        version = "11.1.0";
        base = bases.${version};

        sourceLib = import ./nix/source.nix { inherit pkgs; };

        src = sourceLib.qemuSource {
          patchesRoot = ./patches;
          srcRoot = ./src;
          configsRoot = ./configs;
          buildScript = ./build.sh;
          inherit version base;
        };

        penguin-qemu = pkgs.callPackage ./nix/qemu.nix {
          inherit src;
          version = "${base.tag}-igloo";
        };
      in
      {
        packages = {
          inherit penguin-qemu src;
          default = penguin-qemu;
          # The unpacked release artifact, for CI publish.
          dist = penguin-qemu.dist;
        };

        # `nix flake check` runs the series gate: the patches must apply to the
        # pristine upstream tarball and produce base.json's recorded tree.
        checks.series = pkgs.runCommand "series-applies" { } ''
          test -d ${src}
          test -f ${src}/system/penguin.c
          test -f ${src}/include/system/penguin.h
          test -f ${src}/configs/default.json
          test -x ${src}/build.sh
          touch $out
        '';

        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            git
            python3
            jq
          ];
        };
      }
    );
}
