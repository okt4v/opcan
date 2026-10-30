#pragma once
// Syzygy endgame tablebases: listing, background download, folder checks.
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct TbFile {
    std::string name;   // "KQvK.rtbw"
    int64_t size = -1;  // bytes from the index, -1 if unknown
};

// .rtbw/.rtbz entries of an nginx/apache directory index page.
std::vector<TbFile> parseTbIndex(std::string_view html);

// Number of .rtbw files in dir (0 = no usable tablebases).
int countTables(const std::string& dir);

// Downloads the 3-4-5 piece WDL + DTZ tables into a folder. Complete files are skipped, partial
// ones resumed, so start() after cancel() or a restart continues where it left off.
class TbDownloader {
public:
    ~TbDownloader();
    static constexpr const char* kWdlUrl = "https://tablebase.lichess.ovh/tables/standard/3-4-5-wdl/";
    static constexpr const char* kDtzUrl = "https://tablebase.lichess.ovh/tables/standard/3-4-5-dtz/";
    void start(const std::string& dir);
    void cancel();
    bool running() const;
    bool finished() const;      // every file is complete
    float progress() const;     // 0..1 by bytes
    int64_t total() const;      // bytes, 0 until the index is loaded
    std::string error() const;  // "" if none

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
