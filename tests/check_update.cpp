#include "update.hpp"
#include <cstdio>
#include <cstdlib>

#define CHECK(x) \
    do { \
        if (!(x)) { \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
            std::exit(1); \
        } \
    } while (0)

void checkUpdate() {
    CHECK(compareVersions("0.2.0", "0.1.9") == 1 && compareVersions("v0.10.0", "0.9.9") == 1);
    CHECK(compareVersions("1.0", "1.0.0") == 0 && compareVersions("1.0.0", "v1.0.0") == 0);
    CHECK(compareVersions("1.0.0-beta.1", "1.0.0") == -1 && compareVersions("1.0.0", "1.0.0-rc.1") == 1);
    CHECK(compareVersions("1.0.0-beta.2", "1.0.0-beta.1") == 1 && compareVersions("0.1.0-dev", "0.1.0") == -1);
    CHECK(compareVersions("0.1.0-dev", "0.0.9") == 1);

    const char* body = R"([
      {"tag_name":"v0.2.0","name":"opcan 0.2.0","body":"notes","draft":false,"prerelease":false,"published_at":"2026-11-03T10:00:00Z",
       "assets":[{"name":"opcan-0.2.0-windows-amd64.exe","browser_download_url":"https://x/w.exe","size":10},
                 {"name":"opcan-0.2.0-windows-amd64.zip","browser_download_url":"https://x/w.zip","size":9},
                 {"name":"opcan-0.2.0-darwin-arm64.dmg","browser_download_url":"https://x/a.dmg","size":8},
                 {"name":"opcan-0.2.0-darwin-x86_64.dmg","browser_download_url":"https://x/i.dmg","size":7},
                 {"name":"opcan-0.2.0-linux-x86_64.tar.gz","browser_download_url":"https://x/l.tgz","size":6}]},
      {"tag_name":"v0.10.0-beta.1","draft":false,"prerelease":true,"assets":[]},
      {"tag_name":"v0.3.0","draft":true},
      {"tag_name":"nightly","draft":false},
      {"tag_name":"v0.1.0","draft":false,"assets":"oops"}, 7
    ])";
    auto rs = parseReleases(body);
    CHECK(rs.size() == 3);
    CHECK(rs[0].version == "0.10.0-beta.1" && rs[0].prerelease && rs[1].version == "0.2.0" && rs[2].version == "0.1.0");
    CHECK(rs[1].published == "2026-11-03" && rs[1].notes == "notes" && rs[1].assets.size() == 5);
    CHECK(assetFor(rs[1], "windows-x86_64")->url == "https://x/w.exe");
    CHECK(assetFor(rs[1], "macos-arm64")->url == "https://x/a.dmg");
    CHECK(assetFor(rs[1], "macos-x86_64")->url == "https://x/i.dmg");
    CHECK(assetFor(rs[1], "linux-x86_64")->url == "https://x/l.tgz" && assetFor(rs[1], "linux-x86_64")->size == 6);
    CHECK(!assetFor(rs[0], "linux-x86_64"));
    Release named;  // the release workflow's names
    for (const char* n : {"opcan-0.2.0-windows-setup.exe", "opcan-0.2.0-windows-portable.zip", "opcan-0.2.0-macos-apple-silicon.dmg",
                          "opcan-0.2.0-macos-intel.dmg", "opcan-0.2.0-linux-x86_64.tar.gz"})
        named.assets.push_back({n, n, 1});
    CHECK(assetFor(named, "windows-x86_64")->name == "opcan-0.2.0-windows-setup.exe");
    CHECK(assetFor(named, "macos-arm64")->name == "opcan-0.2.0-macos-apple-silicon.dmg");
    CHECK(assetFor(named, "macos-x86_64")->name == "opcan-0.2.0-macos-intel.dmg");
    CHECK(assetFor(named, "linux-x86_64")->name == "opcan-0.2.0-linux-x86_64.tar.gz");
    CHECK(parseReleases("not json").empty() && parseReleases("{}").empty());
    CHECK(std::string(appVersion()).size() > 0);
    std::puts("update checks passed");
}
