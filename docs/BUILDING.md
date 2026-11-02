# Building opcan

## Development

With Nix (Linux):

    nix develop                      # or direnv
    cmake -S . -B build -G Ninja
    cmake --build build
    ./build/opcan                    # assets are copied next to the binary
    ./build/opcan_check              # self-checks

Without Nix you need CMake >= 3.24, a C++20 compiler, OpenGL headers and (Linux) dbus, wayland and X11
dev packages; everything else is fetched by CMake. GLFW comes from the system if it is >= 3.4, otherwise
it is fetched. The AI coach uses Vulkan when the Vulkan SDK (with `glslc`) is found (`-DOPCAN_VULKAN=OFF`
to skip it) and Metal on macOS. Stockfish is found in `engines/` next to the assets, else on `PATH`.

## Packages

Linux (Nix, primary): `nix build` (result in `./result/bin/opcan`, with a desktop entry and icons),
`nix run`, or `nix profile install .`. Note that a flake only sees files tracked by git, so `git add`
new files before building. Dependency pins live in `packaging/package.nix`; keep them in sync with
`CMakeLists.txt`.

Installers are made with CPack from a normal build. Bundling Stockfish (GPLv3, shipped with its license)
is optional: the fetch scripts download the pinned official release and check its sha256.

    # Windows (Developer PowerShell; NSIS 3 on PATH for the installer)
    packaging\fetch-stockfish.ps1 -Out packaging\stockfish
    cmake -S . -B build -DOPCAN_STOCKFISH_PATH="$PWD/packaging/stockfish/stockfish.exe"
    cmake --build build --config Release
    cd build; cpack -C Release            # opcan-<ver>-windows-amd64.exe (installer) + .zip

    # macOS (.app in a drag-and-drop .dmg, ad-hoc signed, not notarized)
    packaging/fetch-stockfish.sh packaging/stockfish
    cmake -S . -B build -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64 -DOPCAN_STOCKFISH_PATH=$PWD/packaging/stockfish/stockfish
    cmake --build build && (cd build && cpack)

    # Linux tarball (bin/, share/opcan/, share/applications/, share/icons/)
    packaging/fetch-stockfish.sh packaging/stockfish
    cmake -S . -B build -G Ninja -DOPCAN_STOCKFISH_PATH=$PWD/packaging/stockfish/stockfish
    cmake --build build && (cd build && cpack)

To install without CPack use `cmake --install build --prefix <dir> --component opcan` (the component
keeps the dependencies' own headers and libraries out). `.github/workflows/release.yml` does all three
platforms on a `v*` tag. The macOS .dmg is unsigned for Gatekeeper: first launch via right-click > Open,
or `xattr -dr com.apple.quarantine /Applications/opcan.app`.

Icons: `packaging/opcan.svg` is the source; `opcan.ico` and `opcan.icns` were generated from it
(rsvg-convert, then icotool / png2icns) and are committed.

## Where things live

Install layout (found by `resDir()` in `src/engine.cpp`):

| Platform | Program | Assets / bundled engine |
| --- | --- | --- |
| Linux | `<prefix>/bin/opcan` | `<prefix>/share/opcan/{assets,engines}` |
| Windows | `<install dir>\opcan.exe` | `<install dir>\{assets,engines}` |
| macOS | `opcan.app/Contents/MacOS/opcan` | `opcan.app/Contents/Resources/{assets,engines}` |

User data (settings, game database, custom pieces/sounds, AI models):

| Platform | Directory |
| --- | --- |
| Linux | `$XDG_CONFIG_HOME/opcan` (default `~/.config/opcan`) |
| Windows | `%APPDATA%\opcan` |
| macOS | `~/Library/Application Support/opcan` |
