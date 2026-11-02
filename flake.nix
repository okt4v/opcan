{
  description = "opcan - open chess analysis";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (s: f nixpkgs.legacyPackages.${s});
    in {
      packages = forAll (pkgs: rec {
        opcan = pkgs.callPackage ./packaging/package.nix { src = self; };
        default = opcan;
      });
      apps = forAll (pkgs: {
        default = { type = "app"; program = "${self.packages.${pkgs.stdenv.hostPlatform.system}.opcan}/bin/opcan"; };
      });
      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [ cmake ninja pkg-config gcc14 glfw libGL stockfish curl dbus wayland wayland-scanner libffi systemdLibs vulkan-headers vulkan-loader shaderc ];
          # GLFW dlopens wayland/x11/GL and miniaudio dlopens pulse/alsa at runtime
          LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath (with pkgs; [ libGL vulkan-loader alsa-lib libpulseaudio wayland libxkbcommon libx11 libxcursor libxi libxrandr ]);
        };
        # Windows cross build (x86_64 only): packaging/build-windows.sh -> dist/opcan-<ver>-windows-x64-setup.exe
        windows = pkgs.pkgsCross.mingwW64.mkShell {
          nativeBuildInputs = with pkgs; [ cmake ninja nsis curl unzip ];
          buildInputs = [ pkgs.pkgsCross.mingwW64.windows.pthreads ];
        };
      });
    };
}
