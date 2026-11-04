#include "update.hpp"
#include "engine.hpp"
#include "sync.hpp"
#include <nlohmann/json.hpp>
#include <subprocess.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

#ifndef OPCAN_VERSION
#define OPCAN_VERSION "0.0.0"
#endif

const char* appVersion() { return OPCAN_VERSION; }

std::string psQuote(const std::string& s) {
    std::string r = "'";
    for (size_t i = 0; i < s.size(); i++) {
        size_t n = 1;
        bool quote = s[i] == '\'';
        // PowerShell also ends a '...' string at the curly quotes U+2018..U+201B (UTF-8 E2 80 98..9B): double those too
        if (s.compare(i, 2, "\xE2\x80") == 0 && i + 2 < s.size() && (unsigned char)s[i + 2] >= 0x98 && (unsigned char)s[i + 2] <= 0x9B)
            n = 3, quote = true;
        std::string c = s.substr(i, n);
        r += quote ? c + c : c;
        i += n - 1;
    }
    return r + "'";
}

std::string shQuote(const std::string& s) {
    std::string r = "'";
    for (char c : s) r += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return r + "'";
}

namespace {
std::string stripV(const std::string& s) { return !s.empty() && (s[0] == 'v' || s[0] == 'V') ? s.substr(1) : s; }
bool isVersion(const std::string& s) { return !s.empty() && std::isdigit((unsigned char)s[0]); }
std::string str(const json& j, const char* k) {
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : "";
}
// the helper script's quoting: PowerShell on Windows, sh elsewhere
std::string sq(const std::string& s) {
#ifdef _WIN32
    return psQuote(s);
#else
    return shQuote(s);
#endif
}
bool writable(const fs::path& dir) {
    auto probe = dir / ".opcan-write-test";
    std::ofstream f(probe);
    bool ok = bool(f);
    f.close();
    std::error_code ec;
    fs::remove(probe, ec);
    return ok;
}
int myPid() {
#ifdef _WIN32
    return _getpid();
#else
    return (int)getpid();
#endif
}
// Starts a helper that outlives opcan (its output goes nowhere).
bool spawnDetached(const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    for (auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    subprocess_s p{};
    int opts = subprocess_option_inherit_environment | subprocess_option_search_user_path | subprocess_option_no_window;
    if (subprocess_create(argv.data(), opts, &p) != 0) return false;
    return true;  // ponytail: never joined; the helper is reaped by init / Windows once opcan is gone
}
}  // namespace

int compareVersions(const std::string& a0, const std::string& b0) {
    std::string a = stripV(a0), b = stripV(b0);
    auto split = [](const std::string& s, std::string& pre) {
        auto dash = s.find('-');
        pre = dash == std::string::npos ? "" : s.substr(dash + 1);
        std::vector<long> nums;
        std::string core = s.substr(0, dash);
        for (size_t i = 0; i < core.size();) {
            size_t j = core.find('.', i);
            if (j == std::string::npos) j = core.size();
            nums.push_back(std::atol(core.substr(i, j - i).c_str()));
            i = j + 1;
        }
        return nums;
    };
    std::string pa, pb;
    auto na = split(a, pa), nb = split(b, pb);
    for (size_t i = 0; i < std::max(na.size(), nb.size()); i++) {
        long x = i < na.size() ? na[i] : 0, y = i < nb.size() ? nb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    if (pa == pb) return 0;
    if (pa.empty()) return 1;  // the release after its pre-releases
    if (pb.empty()) return -1;
    return pa < pb ? -1 : 1;
}

std::vector<Release> parseReleases(const std::string& body) {
    std::vector<Release> out;
    auto j = json::parse(body, nullptr, false);
    if (!j.is_array()) return out;
    for (auto& x : j) {
        if (!x.is_object() || x.value("draft", false)) continue;
        Release r;
        r.tag = str(x, "tag_name");
        r.version = stripV(r.tag);
        if (!isVersion(r.version)) continue;
        r.name = str(x, "name");
        r.notes = str(x, "body");
        r.published = str(x, "published_at").substr(0, 10);
        r.prerelease = x.value("prerelease", false);
        if (x.contains("assets") && x["assets"].is_array())
            for (auto& a : x["assets"])
                if (a.is_object())
                    r.assets.push_back({str(a, "name"), str(a, "browser_download_url"),
                                        a.contains("size") && a["size"].is_number() ? a["size"].get<int64_t>() : 0});
        out.push_back(std::move(r));
    }
    std::stable_sort(out.begin(), out.end(), [](const Release& a, const Release& b) { return compareVersions(a.version, b.version) > 0; });
    return out;
}

std::vector<Release> fetchReleases(std::string* err) {
    auto body = httpGet("https://api.github.com/repos/okt4v/opcan/releases?per_page=50",
                        {"Accept: application/vnd.github+json"}, err);
    if (!body) return {};
    auto r = parseReleases(*body);
    if (r.empty() && err && err->empty()) *err = "no releases yet";
    return r;
}

std::vector<NoteLine> plainNotes(const std::string& md) {
    std::vector<NoteLine> out;
    bool skip = false;
    size_t a = 0;
    while (a <= md.size()) {
        size_t b = md.find('\n', a);
        if (b == std::string::npos) b = md.size();
        std::string l = md.substr(a, b - a);
        a = b + 1;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        size_t i = l.find_first_not_of(" \t");
        l = i == std::string::npos ? "" : l.substr(i);
        bool heading = !l.empty() && l[0] == '#';
        if (heading) {
            l = l.substr(std::min(l.size(), l.find_first_not_of('#')));
            l = l.substr(std::min(l.size(), l.find_first_not_of(' ')));
            std::string low = l;
            for (auto& c : low) c = (char)std::tolower((unsigned char)c);
            skip = low.rfind("download", 0) == 0 || low.rfind("install", 0) == 0;  // the release page's table
        }
        if (skip || (!l.empty() && l[0] == '|')) continue;
        if (l.rfind("- ", 0) == 0 || l.rfind("* ", 0) == 0) l = "• " + l.substr(2);
        std::string t;  // [text](url) -> text, drop ** and `
        for (size_t k = 0; k < l.size(); k++) {
            if (l[k] == '[') {
                size_t close = l.find("](", k), end = close == std::string::npos ? close : l.find(')', close);
                if (end != std::string::npos) {
                    t += l.substr(k + 1, close - k - 1);
                    k = end;
                    continue;
                }
            }
            if (l[k] == '`' || (l[k] == '*' && k + 1 < l.size() && l[k + 1] == '*')) {
                k += l[k] == '*';
                continue;
            }
            t += l[k];
        }
        if (t.empty() && (out.empty() || out.back().text.empty() || out.back().heading)) continue;  // no stray blank lines
        out.push_back({heading, t});
    }
    while (!out.empty() && out.back().text.empty()) out.pop_back();
    return out;
}

std::string thisPlatform() {
#if defined(_WIN32)
    return "windows-x86_64";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "macos-arm64";
#elif defined(__APPLE__)
    return "macos-x86_64";
#else
    return "linux-x86_64";
#endif
}

// Release downloads (release.yml): opcan-<v>-windows-setup.exe / -windows-portable.zip, -macos-apple-silicon.dmg,
// -macos-intel.dmg, -linux-x86_64.tar.gz; CPack's own names (windows-amd64, darwin-<arch>) match too
const Release::Asset* assetFor(const Release& r, const std::string& platform) {
    auto low = [](std::string s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    auto ends = [](const std::string& s, const std::string& e) { return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0; };
    for (auto& a : r.assets) {
        std::string n = low(a.name);
        bool win = n.find("win") != std::string::npos && ends(n, ".exe");
        bool arm = n.find("arm64") != std::string::npos || n.find("aarch64") != std::string::npos ||
                   n.find("apple-silicon") != std::string::npos;
        bool mac = (n.find("darwin") != std::string::npos || n.find("macos") != std::string::npos) && ends(n, ".dmg");
        bool lin = n.find("linux") != std::string::npos && ends(n, ".tar.gz") && !arm;
        if ((platform == "windows-x86_64" && win) || (platform == "macos-arm64" && mac && arm) ||
            (platform == "macos-x86_64" && mac && !arm) || (platform == "linux-x86_64" && lin))
            return &a;
    }
    return nullptr;
}

namespace {
// A build tree (CMakeCache.txt next to the binary or a few levels up) is never overwritten.
bool devBuild(fs::path p) {
    std::error_code ec;
    for (int i = 0; i < 4 && !p.empty(); i++, p = p.parent_path())
        if (fs::exists(p / "CMakeCache.txt", ec)) return true;
    return false;
}
#if !defined(_WIN32) && !defined(__APPLE__)
bool onPath(const std::string& exe) {
    const char* path = std::getenv("PATH");
    std::string p = path ? path : "";
    std::error_code ec;
    for (size_t a = 0, b; a < p.size(); a = b + 1) {
        b = p.find(':', a);
        if (b == std::string::npos) b = p.size();
        if (b > a && fs::exists(fs::path(p.substr(a, b - a)) / exe, ec)) return true;
    }
    return false;
}
#endif
// The Windows portable .zip of r
const Release::Asset* portableZip(const Release& r) {
    for (auto& a : r.assets) {
        std::string n = a.name;
        for (auto& c : n) c = (char)std::tolower((unsigned char)c);
        if (n.find("win") != std::string::npos && n.size() > 4 && n.compare(n.size() - 4, 4, ".zip") == 0) return &a;
    }
    return nullptr;
}
}  // namespace

namespace {
InstallKind detectInstallKind() {
    std::error_code ec;
    fs::path exe = exeDir();
    if (devBuild(exe)) {
#if !defined(_WIN32) && !defined(__APPLE__)
        if (fs::exists("/etc/NIXOS", ec)) return onPath("nix") ? InstallKind::Nix : InstallKind::None;  // generic builds don't run there
#endif
        return InstallKind::Separate;
    }
#if defined(_WIN32)
    // opcan.nsi's per-user install; an old CPack install in Program Files (not writable) updates as Separate, i.e. per-user
    if (fs::exists(exe / "Uninstall.exe", ec) && writable(exe)) return InstallKind::Installer;
    if (fs::exists(exe / "opcan.exe", ec) && writable(exe)) return InstallKind::Portable;
#elif defined(__APPLE__)
    fs::path app = exe.parent_path().parent_path();  // opcan.app/Contents/MacOS
    if (app.extension() == ".app" && writable(app.parent_path())) return InstallKind::AppBundle;
#else
    if (exe.string().rfind("/nix/store", 0) == 0 || fs::exists("/etc/NIXOS", ec))
        return onPath("nix") ? InstallKind::Nix : InstallKind::None;
    fs::path root = exe.parent_path();
    if (exe.filename() == "bin" && fs::is_directory(root / "share/opcan/assets", ec) && writable(root)) return InstallKind::Tarball;
#endif
    return InstallKind::Separate;
}
}  // namespace

// the layout doesn't change while opcan runs; detecting it probes writability with a file (the UI asks every frame)
InstallKind installKind() {
    static const InstallKind k = detectInstallKind();
    return k;
}

bool installable(const Release& r) {
    InstallKind k = installKind();
    if (k == InstallKind::None) return false;
    if (k == InstallKind::Nix) return true;
    if (k == InstallKind::Portable) return portableZip(r) != nullptr;
    return assetFor(r, thisPlatform()) != nullptr;
}

bool replacesInPlace(InstallKind k) {
    return k == InstallKind::Installer || k == InstallKind::AppBundle || k == InstallKind::Tarball || k == InstallKind::Portable;
}

const char* installKindText(InstallKind k) {
    switch (k) {
        case InstallKind::Installer: return "Windows installer";
        case InstallKind::AppBundle: return "macOS app";
        case InstallKind::Tarball: return "portable Linux package";
        case InstallKind::Portable: return "portable Windows copy";
#if defined(_WIN32)
        case InstallKind::Separate: return "other versions are installed for your user and start from there";
#elif defined(__APPLE__)
        case InstallKind::Separate: return "other versions are installed in ~/Applications and start from there";
#else
        case InstallKind::Separate: return "other versions are installed in ~/.local/opt/opcan and start from there";
#endif
        case InstallKind::Nix: return "Nix: other versions are built with Nix (can take a while) and start from there";
        default: return "this copy can't install other versions (no nix command): get them from GitHub";
    }
}

std::string prepareInstall(const Release& r, bool silent, bool relaunch) {
    InstallKind kind = installKind();
    if (kind == InstallKind::None) return "this copy of opcan can't update itself";
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / ("opcan-update-" + r.version);
    fs::create_directories(dir, ec);
    fs::path exe = exeDir();
    int pid = myPid();
#if !defined(_WIN32)
    std::string wait = "exec >/dev/null 2>&1\nwhile kill -0 " + std::to_string(pid) + " 2>/dev/null; do sleep 0.3; done\n";
    if (kind == InstallKind::Nix) {  // build that release's flake, keep it from the garbage collector, start it
        const char* home = std::getenv("HOME");
        fs::path state = fs::path(home ? home : "/tmp") / ".local/state/opcan";
        fs::create_directories(state, ec);
        std::string link = (state / ("nix-" + r.version)).string();
        std::string flake = "github:okt4v/opcan/" + r.tag;
        const char* argv[] = {"nix", "--extra-experimental-features", "nix-command flakes", "build", flake.c_str(),
                              "--out-link", link.c_str(), nullptr};
        subprocess_s p{};
        int opts = subprocess_option_inherit_environment | subprocess_option_search_user_path | subprocess_option_combined_stdout_stderr;
        if (subprocess_create(argv, opts, &p) != 0) return "couldn't run nix";
        std::string out;
        char buf[512];
        while (unsigned n = subprocess_read_stdout(&p, buf, sizeof buf)) out.append(buf, n);
        int code = 1;
        subprocess_join(&p, &code);
        subprocess_destroy(&p);
        if (code != 0 || !fs::exists(fs::path(link) / "bin/opcan", ec)) {
            auto e = out.rfind("error:");
            return "nix build failed" + (e == std::string::npos ? std::string() : ": " + out.substr(e + 7, 200));
        }
        std::string script = wait + (relaunch ? sq(link + "/bin/opcan") + " &\n" : "");
        fs::path sh = dir / "install.sh";
        std::ofstream(sh) << script;
        return spawnDetached({"/bin/sh", sh.string()}) ? "" : "could not start the new version";
    }
#endif
    auto* a = kind == InstallKind::Portable ? portableZip(r) : assetFor(r, thisPlatform());
    if (!a) return "release " + r.version + " has no package for " + thisPlatform();
    fs::path pkg = dir / a->name;
    std::string err;
    if (!httpDownload(a->url, pkg.string(), &err)) return "download failed: " + err;
    if (a->size && (int64_t)fs::file_size(pkg, ec) != a->size) return "download incomplete";

#if defined(_WIN32)
    // PowerShell (Windows 10+): wait for opcan to exit, install, start the new opcan
    std::string ps = "Wait-Process -Id " + std::to_string(pid) + " -ErrorAction SilentlyContinue; ";
    if (kind == InstallKind::Portable) {  // unzip, then copy the folder holding opcan.exe over this one
        // -LiteralPath: '[' in a path is a wildcard for -Path
        std::string x = (dir / "x").string();
        ps += "Remove-Item -Recurse -Force -ErrorAction SilentlyContinue -LiteralPath " + sq(x) + "; Expand-Archive -Force -LiteralPath " +
              sq(pkg.string()) + " -DestinationPath " + sq(x) + "; $s = (Get-ChildItem -Recurse -Filter opcan.exe -LiteralPath " + sq(x) +
              " | Select-Object -First 1).DirectoryName; if ($s) { Get-ChildItem -LiteralPath $s | Copy-Item -Recurse -Force -Destination " +
              sq(exe.string()) + " }; " + (relaunch ? "Start-Process -FilePath " + sq((exe / "opcan.exe").string()) : std::string());
    } else if (kind == InstallKind::Installer) {  // packaging/opcan.nsi over this install: /D= keeps it in this directory
        // (Start-Process joins the arguments unquoted, which is what NSIS wants: /D= last, spaces and all)
        ps += "Start-Process -Wait -FilePath " + sq(pkg.string()) + " -ArgumentList " + sq(std::string(silent ? "/S " : "") + "/D=" + exe.string()) +
              "; " + (relaunch ? "Start-Process -FilePath " + sq((exe / "opcan.exe").string()) : std::string());
    } else {  // Separate: the per-user installer to its own directory (opcan.nsi: HKCU\Software\opcan InstallDir, else
              // %LOCALAPPDATA%\Programs\opcan), then start that copy
        ps += "Start-Process -Wait -FilePath " + sq(pkg.string()) + (silent ? " -ArgumentList '/S'" : "") + "; ";
        if (relaunch)
            ps += "$d = (Get-ItemProperty -Path 'HKCU:\\Software\\opcan' -ErrorAction SilentlyContinue).InstallDir; "
                  "if (-not $d) { $d = \"$env:LOCALAPPDATA\\Programs\\opcan\" }; Start-Process -FilePath \"$d\\opcan.exe\"";
    }
    if (!spawnDetached({"powershell", "-NoProfile", "-WindowStyle", "Hidden", "-Command", ps})) return "could not start the installer";
#else
    (void)silent;
    std::string script = wait;
    const char* home = std::getenv("HOME");
    if (thisPlatform().rfind("macos", 0) == 0) {  // AppBundle: replace this .app; Separate: ~/Applications/opcan.app
        fs::path app = kind == InstallKind::AppBundle ? exe.parent_path().parent_path() : fs::path(home ? home : "/tmp") / "Applications/opcan.app";
        script += "mkdir -p " + sq(app.parent_path().string()) + "\n";
        std::string mnt = (dir / "mnt").string();
        script += "mkdir -p " + sq(mnt) + "\nhdiutil attach -nobrowse -noautoopen -mountpoint " + sq(mnt) + " " + sq(pkg.string()) +
                  " || exit 1\nrm -rf " + sq(app.string() + ".new") + "\ncp -R " + sq(mnt + "/opcan.app") + " " +
                  sq(app.string() + ".new") + " && rm -rf " + sq(app.string()) + " && mv " + sq(app.string() + ".new") + " " +
                  sq(app.string()) + "\nhdiutil detach " + sq(mnt) + "\n" + (relaunch ? "open " + sq(app.string()) + "\n" : "");
    } else {  // Tarball: unpack over the install root (bin/, share/); Separate: into ~/.local/opt/opcan, plus a menu entry
        fs::path root = kind == InstallKind::Tarball ? exe.parent_path() : fs::path(home ? home : "/tmp") / ".local/opt/opcan";
        if (kind == InstallKind::Separate) {
            fs::path apps = fs::path(home ? home : "/tmp") / ".local/share/applications";
            script += "mkdir -p " + sq(root.string()) + " " + sq(apps.string()) + "\n";
            script += "printf '%s\\n' '[Desktop Entry]' 'Type=Application' 'Name=opcan' 'Comment=Chess game review' " +
                      sq("Exec=" + (root / "bin/opcan").string() + " %F") + " " +
                      sq("Icon=" + (root / "share/icons/hicolor/scalable/apps/opcan.svg").string()) +
                      " 'Categories=Game;BoardGame;' 'StartupWMClass=opcan' > " + sq((apps / "opcan.desktop").string()) + "\n";
        }
        std::string x = (dir / "x").string();
        script += "rm -rf " + sq(x) + "\nmkdir -p " + sq(x) + "\ntar -xzf " + sq(pkg.string()) + " -C " + sq(x) + " || exit 1\n" +
                  "src=$(find " + sq(x) + " -mindepth 1 -maxdepth 1 -type d | head -n 1)\n" +
                  "[ -x \"$src/bin/opcan\" ] || exit 1\ncp -a \"$src\"/. " + sq(root.string()) + "/ || exit 1\nrm -rf " + sq(x) + "\n" +
                  (relaunch ? sq((root / "bin/opcan").string()) + " &\n" : "");
    }
    fs::path sh = dir / "install.sh";
    std::ofstream(sh) << script;
    if (!spawnDetached({"/bin/sh", sh.string()})) return "could not start the installer";
#endif
    return "";
}
