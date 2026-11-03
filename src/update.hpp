#pragma once
// Updates from the GitHub releases of okt4v/opcan: check, pick this platform's package, install it.
#include <cstdint>
#include <string>
#include <vector>

inline constexpr const char* kReleasesPage = "https://github.com/okt4v/opcan/releases";

struct Release {
    std::string tag, version;  // "v0.2.0", "0.2.0"
    std::string name, notes, published;  // published: "2026-11-03"
    bool prerelease = false;
    struct Asset {
        std::string name, url;
        int64_t size = 0;
    };
    std::vector<Asset> assets;
};

const char* appVersion();  // this build's version, from the release tag (local builds: the CMake project version)

// -1 / 0 / 1. Numeric parts compare as numbers ("0.10" > "0.9"); a pre-release ("1.0.0-beta.2") sorts before its
// release, and pre-release tags compare as text. A leading "v" is ignored.
int compareVersions(const std::string& a, const std::string& b);

// GitHub's /releases JSON; drafts and releases with a tag that isn't a version are skipped. Newest version first.
std::vector<Release> parseReleases(const std::string& json);
std::vector<Release> fetchReleases(std::string* err);  // blocking

// How this copy was installed, i.e. how the updater installs another version.
enum class InstallKind {
    None,       // NixOS / a Nix package without the nix command: updates are only announced
    Installer,  // Windows: the NSIS installer's directory (has Uninstall.exe)
    AppBundle,  // macOS: opcan.app in a writable folder
    Tarball,    // Linux: the portable .tar.gz unpacked in a writable folder (bin/opcan + share/opcan)
    Portable,   // Windows: the portable .zip unpacked in a writable folder
    Separate,   // a dev build or a copy opcan can't write to: the release goes to a per-user location
                // (Windows: the per-user installer, macOS: ~/Applications, Linux: ~/.local/opt/opcan) and starts from there
    Nix,        // NixOS or a Nix package: `nix build github:okt4v/opcan/<tag>` and start the result
};
InstallKind installKind();
const char* installKindText(InstallKind k);  // "Windows installer", ...
bool installable(const Release& r);          // the updater can install r on this copy (else: the release page)
bool replacesInPlace(InstallKind k);         // the update replaces this copy (automatic updates are only for those)

// This platform's package in r (nullptr if the release has none). For tests: the platform is passed in.
const Release::Asset* assetFor(const Release& r, const std::string& platform);
std::string thisPlatform();  // "windows-x86_64", "macos-arm64", "macos-x86_64", "linux-x86_64"

// Downloads (Nix: builds) r's package and starts a helper that waits for opcan to exit, installs it and (relaunch) starts the new
// version. silent: no installer UI (Windows). Returns "" when the helper is running; the caller should quit
// (now, or whenever the user does). Blocking (the download): run it off the UI thread.
std::string prepareInstall(const Release& r, bool silent, bool relaunch);
