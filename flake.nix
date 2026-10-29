{
  description = "opcan - open chess analysis";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (s: f nixpkgs.legacyPackages.${s});
    in {
      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [ cmake ninja pkg-config gcc14 glfw libGL stockfish curl ];
          # GLFW dlopens wayland/x11/GL at runtime
          LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath (with pkgs; [ libGL wayland libxkbcommon libx11 libxcursor libxi libxrandr ]);
        };
      });
    };
}
