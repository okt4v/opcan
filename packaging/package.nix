# Nix package for opcan (used by flake.nix: `nix build`, `nix run`).
# CMake's FetchContent can't download in the sandbox, so every dependency is pinned here and handed
# over via FETCHCONTENT_SOURCE_DIR_<NAME>. Keep these in sync with the FetchContent_Declare()s in CMakeLists.txt.
{ lib, gcc14Stdenv, src, fetchFromGitHub, fetchzip, cmake, ninja, pkg-config, makeWrapper, librsvg, shaderc
, glfw, libGL, dbus, systemdLibs, libffi, wayland-scanner, vulkan-headers, vulkan-loader, alsa-lib, libpulseaudio, wayland, libxkbcommon
, libx11, libxcursor, libxi, libxrandr, stockfish, curl }:
let
  gh = owner: repo: rev: hash: fetchFromGitHub { inherit owner repo rev hash; };
  deps = {
    IMGUI = gh "ocornut" "imgui" "v1.93.0-docking" "sha256-NCD0li4ECPSFhhU0egSWMiYNBKSJDoB80rSOUiRAQ6c=";
    CHESSLIB = gh "Disservin" "chess-library" "3af102c53752472c208d48921ece705b1f0d1626" "sha256-/S/yDcdpWejfA1I1Xbl2VdHkZ7i1fHZoLcPx3CCAVZU=";
    SUBPROCESS = gh "sheredom" "subprocess.h" "dd28a9cca5eb6783de8bf7879599e04197472fe0" "sha256-UXF9x6WJ3kfGvloJHXgQoaZ2T+qSBKf0Qv/S0Scf5qU=";
    LUNASVG = gh "sammycage" "lunasvg" "v3.5.0" "sha256-eSkYkxdV5L31cIJtH6cVfQU2nguA3BPCQXnIMnColek=";
    STB = gh "nothings" "stb" "003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5" "sha256-tvt5gGQjeYNaoYzKgN6dPDtfN1Ylmqae5tYGYNsoi1M=";
    SQLITE = fetchzip { url = "https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip"; hash = "sha256-NFLUmOWa5iXvBsM98QW3nw/PulG9H3iI6V9vMnpcBK4="; };
    JSON = fetchzip { url = "https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz"; hash = "sha256-tOInM4YYi/cbHCWP2R0jlFzlaTN0ZVgHFMWagz9Wnvk="; };
    NFD = fetchFromGitHub {  # wayland-protocols is a git submodule
      owner = "btzy"; repo = "nativefiledialog-extended"; rev = "v1.4.1"; fetchSubmodules = true;
      hash = "sha256-zyVosxjbW4lHT/SNFr0PbeY5conpn9hTfZkxn2kxDQE=";
    };
    MINIAUDIO = gh "mackron" "miniaudio" "0.11.25" "sha256-2k346Z/ueINPbaY20P2cbBvRfFXXH0ugdv4d7WaYt2w=";
    LLAMA = gh "ggml-org" "llama.cpp" "b11850" "sha256-LhwuuyMdzMXvxxVUKgyhYpV+dj5WJEoFlSilFDKmvd0=";
  };
  # GLFW dlopens wayland/x11/GL, miniaudio dlopens pulse/alsa, ggml dlopens the Vulkan loader
  runtimeLibs = [ libGL vulkan-loader alsa-lib libpulseaudio wayland libxkbcommon libx11 libxcursor libxi libxrandr ];
in
gcc14Stdenv.mkDerivation {
  pname = "opcan";
  version = "0.1.0";
  inherit src;

  nativeBuildInputs = [ cmake ninja pkg-config makeWrapper librsvg shaderc wayland-scanner ];
  buildInputs = [ glfw libGL dbus systemdLibs wayland libffi vulkan-headers vulkan-loader ];  # nfd: dbus + wayland

  cmakeFlags = [
    "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
    "-DOPCAN_VULKAN=ON"
  ] ++ lib.mapAttrsToList (name: dir: "-DFETCHCONTENT_SOURCE_DIR_${name}=${dir}") deps;

  # only our component: llama.cpp/lunasvg/nfd/json would otherwise install their headers and static libs
  installPhase = ''
    runHook preInstall
    cmake --install . --prefix $out --component opcan
    runHook postInstall
  '';

  postInstall = ''
    for s in 64 128 256 512; do
      mkdir -p $out/share/icons/hicolor/''${s}x''${s}/apps
      rsvg-convert -w $s -h $s ${./opcan.svg} -o $out/share/icons/hicolor/''${s}x''${s}/apps/opcan.png
    done
    # stockfish and curl (game sync) are looked up on PATH; the user's own PATH wins
    wrapProgram $out/bin/opcan \
      --suffix PATH : ${lib.makeBinPath [ stockfish curl ]} \
      --suffix LD_LIBRARY_PATH : ${lib.makeLibraryPath runtimeLibs}
  '';

  doInstallCheck = true;
  installCheckPhase = ''
    test -f $out/share/opcan/assets/fonts/Inter-Regular.ttf
    test -f $out/share/applications/opcan.desktop
  '';

  meta = {
    description = "Open chess analysis: game review with Stockfish and a local AI coach";
    mainProgram = "opcan";
    license = lib.licenses.gpl3Plus;
    platforms = lib.platforms.linux;
  };
}
