#include "tablebase.hpp"
#include "sync.hpp"
#include <subprocess.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

std::vector<TbFile> parseTbIndex(std::string_view html) {
    std::vector<TbFile> out;
    for (size_t i = 0; (i = html.find("<a href=\"", i)) != std::string_view::npos;) {
        i += 9;
        size_t q = html.find('"', i);
        if (q == std::string_view::npos) break;
        std::string_view name = html.substr(i, q - i);
        if (!(name.ends_with(".rtbw") || name.ends_with(".rtbz")) || name.find_first_of("/?&<>") != std::string_view::npos) continue;
        // nginx autoindex: <a href="X">X</a>   date time   size
        size_t a = html.find("</a>", q), e = html.find('\n', q);
        std::string_view rest = a < e ? html.substr(a, e - a) : std::string_view{};
        while (!rest.empty() && std::isspace((unsigned char)rest.back())) rest.remove_suffix(1);
        size_t k = rest.find_last_of(' ');
        std::string_view num = k == std::string_view::npos ? std::string_view{} : rest.substr(k + 1);
        bool digits = !num.empty() && std::all_of(num.begin(), num.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (std::none_of(out.begin(), out.end(), [&](auto& f) { return f.name == name; }))
            out.push_back({std::string(name), digits ? std::stoll(std::string(num)) : -1});
    }
    return out;
}

int countTables(const std::string& dir) {
    std::error_code ec;
    int n = 0;
    if (dir.empty()) return 0;
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".rtbw") n++;
    return n;
}

struct TbDownloader::Impl {
    std::string dir;
    std::atomic<bool> cancel{false}, running{false}, finished{false};
    std::atomic<int64_t> total{0}, done{0};
    std::string partPath;  // file being downloaded, for live progress
    mutable std::mutex m;
    std::string err;

    void fail(std::string e) {
        std::lock_guard l(m);
        err = std::move(e);
    }
    // curl into path (resuming), killable via cancel. No overall timeout: big files on slow links are fine.
    bool fetch(const std::string& url, const std::string& path) {
        const char* argv[] = {"curl", "-sS", "-L", "-f", "--retry", "3", "--speed-limit", "1024", "--speed-time", "30",
                              "-C", "-", "-A", "opcan/0.1 (local chess analysis app)", "-o", path.c_str(), url.c_str(), nullptr};
        subprocess_s p{};
        int opts = subprocess_option_inherit_environment | subprocess_option_search_user_path | subprocess_option_no_window;
        if (subprocess_create(argv, opts, &p) != 0) return fail("could not run curl"), false;
        while (subprocess_alive(&p)) {
            if (cancel) subprocess_terminate(&p);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        int rc = -1;
        subprocess_join(&p, &rc);
        subprocess_destroy(&p);
        if (cancel) return false;
        if (rc != 0) return fail("download failed (curl " + std::to_string(rc) + ")"), false;
        return true;
    }
    void run() {
        std::vector<TbFile> files;
        for (const char* url : {kWdlUrl, kDtzUrl}) {
            std::string e;
            auto html = httpGet(url, {}, &e);
            if (!html) return fail("could not list the tables: " + e);
            for (auto& f : parseTbIndex(*html)) files.push_back(f);
        }
        if (files.size() < 2) return fail("the table list is empty");
        int64_t sum = 0;
        for (auto& f : files) sum += std::max<int64_t>(0, f.size);
        total = sum;
        std::error_code ec;
        fs::create_directories(dir, ec);
        for (auto& f : files) {
            if (cancel) return;
            fs::path out = fs::path(dir) / f.name, part = out;
            part += ".part";
            auto size = fs::file_size(out, ec);
            if (!ec && (f.size < 0 || (int64_t)size == f.size)) {
                done += std::max<int64_t>(0, f.size);
                continue;
            }
            if (auto ps = fs::file_size(part, ec); !ec && f.size >= 0 && (int64_t)ps >= f.size) fs::remove(part, ec);
            {
                std::lock_guard l(m);
                partPath = part.string();
            }
            const char* base = f.name.ends_with(".rtbw") ? kWdlUrl : kDtzUrl;
            if (!fetch(base + f.name, part.string())) return;
            auto got = fs::file_size(part, ec);
            if (ec || (f.size >= 0 && (int64_t)got != f.size)) {
                fs::remove(part, ec);
                return fail(f.name + ": size mismatch");
            }
            fs::rename(part, out, ec);
            if (ec) return fail("could not write " + out.string());
            done += std::max<int64_t>(0, f.size);
        }
        std::lock_guard l(m);
        partPath.clear();
        finished = true;
    }
};

TbDownloader::~TbDownloader() { cancel(); }

void TbDownloader::start(const std::string& dir) {
    cancel();
    impl_ = std::make_shared<Impl>();
    impl_->dir = dir;
    impl_->running = true;
    std::thread([im = impl_] {
        im->run();
        im->running = false;
    }).detach();  // cancel() never blocks the UI; the thread stops curl within 100 ms
}

void TbDownloader::cancel() {
    if (!impl_) return;
    impl_->cancel = true;
    impl_.reset();
}

bool TbDownloader::running() const { return impl_ && impl_->running; }
bool TbDownloader::finished() const { return impl_ && impl_->finished; }
int64_t TbDownloader::total() const { return impl_ ? impl_->total.load() : 0; }

float TbDownloader::progress() const {
    if (!impl_ || impl_->total <= 0) return 0;
    int64_t d = impl_->done;
    {
        std::lock_guard l(impl_->m);
        std::error_code ec;
        if (!impl_->partPath.empty())
            if (auto s = fs::file_size(impl_->partPath, ec); !ec) d += (int64_t)s;
    }
    return std::min(1.f, (float)((double)d / (double)impl_->total));
}

std::string TbDownloader::error() const {
    if (!impl_) return "";
    std::lock_guard l(impl_->m);
    return impl_->err;
}
