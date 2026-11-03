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

namespace {
std::string stripV(const std::string& s) { return !s.empty() && (s[0] == 'v' || s[0] == 'V') ? s.substr(1) : s; }
bool isVersion(const std::string& s) { return !s.empty() && std::isdigit((unsigned char)s[0]); }
std::string str(const json& j, const char* k) {
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : "";
}
// 'it''s' -> shell single-quoted
std::string sq(const std::string& s) {
    std::string r = "'";
    for (char c : s) r += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return r + "'";
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

InstallKind installKind() {
    std::error_code ec;
    fs::path exe = exeDir();
#if defined(_WIN32)
    if (fs::exists(exe / "Uninstall.exe", ec)) return InstallKind::Installer;
#elif defined(__APPLE__)
    fs::path app = exe.parent_path().parent_path();  // opcan.app/Contents/MacOS
    if (app.extension() == ".app" && writable(app.parent_path())) return InstallKind::AppBundle;
#else
    fs::path root = exe.parent_path();
    if (exe.filename() == "bin" && fs::is_directory(root / "share/opcan/assets", ec) &&
        exe.string().rfind("/nix/store", 0) != 0 && writable(root))
        return InstallKind::Tarball;
#endif
    return InstallKind::None;
}

const char* installKindText(InstallKind k) {
    switch (k) {
        case InstallKind::Installer: return "Windows installer";
        case InstallKind::AppBundle: return "macOS app";
        case InstallKind::Tarball: return "portable Linux package";
        default: return "this copy can't update itself (a development build, Nix or a system package)";
    }
}

std::string prepareInstall(const Release& r, bool silent, bool relaunch) {
    InstallKind kind = installKind();
    if (kind == InstallKind::None) return "this copy of opcan can't update itself";
    auto* a = assetFor(r, thisPlatform());
    if (!a) return "release " + r.version + " has no package for " + thisPlatform();
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / ("opcan-update-" + r.version);
    fs::create_directories(dir, ec);
    fs::path pkg = dir / a->name;
    std::string err;
    if (!httpDownload(a->url, pkg.string(), &err)) return "download failed: " + err;
    if (a->size && (int64_t)fs::file_size(pkg, ec) != a->size) return "download incomplete";
    fs::path exe = exeDir();
    int pid = myPid();

#if defined(_WIN32)
    // PowerShell (Windows 10+): wait for opcan to exit, run the installer, start the new opcan
    std::string ps = "Wait-Process -Id " + std::to_string(pid) + " -ErrorAction SilentlyContinue; " +
                     "Start-Process -Wait -FilePath " + sq(pkg.string()) + (silent ? " -ArgumentList '/S'" : "") + "; " +
                     (relaunch ? "Start-Process -FilePath " + sq((exe / "opcan.exe").string()) : std::string());
    if (!spawnDetached({"powershell", "-NoProfile", "-WindowStyle", "Hidden", "-Command", ps})) return "could not start the installer";
#else
    (void)silent;
    std::string script = "exec >/dev/null 2>&1\nwhile kill -0 " + std::to_string(pid) + " 2>/dev/null; do sleep 0.3; done\n";
    if (kind == InstallKind::AppBundle) {
        fs::path app = exe.parent_path().parent_path();
        std::string mnt = (dir / "mnt").string();
        script += "mkdir -p " + sq(mnt) + "\nhdiutil attach -nobrowse -noautoopen -mountpoint " + sq(mnt) + " " + sq(pkg.string()) +
                  " || exit 1\nrm -rf " + sq(app.string() + ".new") + "\ncp -R " + sq(mnt + "/opcan.app") + " " +
                  sq(app.string() + ".new") + " && rm -rf " + sq(app.string()) + " && mv " + sq(app.string() + ".new") + " " +
                  sq(app.string()) + "\nhdiutil detach " + sq(mnt) + "\n" + (relaunch ? "open " + sq(app.string()) + "\n" : "");
    } else {  // Tarball: unpack over the install root (bin/, share/)
        fs::path root = exe.parent_path();
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
