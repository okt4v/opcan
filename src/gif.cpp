#include "gif.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace {

using Bytes = std::vector<unsigned char>;

int bin15(const unsigned char* p) { return (p[0] >> 3) << 10 | (p[1] >> 3) << 5 | p[2] >> 3; }

void put16(Bytes& o, int v) {
    o.push_back(v & 0xFF);
    o.push_back(v >> 8 & 0xFF);
}

// LZW-compress 8-bit indices into GIF sub-blocks (min code size 8).
void lzw(Bytes& o, const Bytes& idx) {
    o.push_back(8);
    Bytes data;
    uint32_t acc = 0;
    int bits = 0, size = 9, next = 258;
    auto emit = [&](int code) {
        acc |= uint32_t(code) << bits;
        for (bits += size; bits >= 8; bits -= 8, acc >>= 8) data.push_back(acc & 0xFF);
    };
    std::unordered_map<int, int> dict;
    emit(256);
    int prefix = idx[0];
    for (size_t i = 1; i < idx.size(); ++i) {
        int key = prefix << 8 | idx[i];
        if (auto it = dict.find(key); it != dict.end()) { prefix = it->second; continue; }
        emit(prefix);
        dict[key] = next++;
        if (next == (1 << size) + 1 && size < 12) ++size;
        if (next == 4096) { // table full: reset
            emit(256);
            dict.clear();
            size = 9, next = 258;
        }
        prefix = idx[i];
    }
    emit(prefix);
    emit(257);
    if (bits > 0) data.push_back(acc & 0xFF);
    for (size_t i = 0; i < data.size(); i += 255) {
        size_t n = std::min<size_t>(255, data.size() - i);
        o.push_back((unsigned char)n);
        o.insert(o.end(), data.begin() + i, data.begin() + i + n);
    }
    o.push_back(0);
}

} // namespace

bool writeGif(const std::string& path, int w, int h, const std::vector<std::vector<unsigned char>>& frames,
              const std::vector<int>& delaysCs, bool loop) {
    if (w <= 0 || h <= 0 || w > 65535 || h > 65535 || frames.empty() || delaysCs.size() != frames.size()) return false;
    const size_t px = size_t(w) * h;
    for (auto& f : frames)
        if (f.size() != px * 4) return false;

    // Popularity palette over 15-bit bins; each entry is the bin's average real colour.
    std::vector<uint64_t> cnt(32768), sum(32768 * 3);
    for (auto& f : frames)
        for (size_t i = 0; i < px; ++i) {
            const unsigned char* p = &f[i * 4];
            int b = bin15(p);
            ++cnt[b];
            for (int c = 0; c < 3; ++c) sum[b * 3 + c] += p[c];
        }
    std::vector<int> bins;
    for (int b = 0; b < 32768; ++b)
        if (cnt[b]) bins.push_back(b);
    size_t ncol = std::min<size_t>(256, bins.size());
    std::partial_sort(bins.begin(), bins.begin() + ncol, bins.end(), [&](int a, int b) { return cnt[a] > cnt[b]; });
    std::array<unsigned char, 768> pal{};
    for (size_t i = 0; i < ncol; ++i)
        for (int c = 0; c < 3; ++c) pal[i * 3 + c] = (unsigned char)(sum[bins[i] * 3 + c] / cnt[bins[i]]);

    // Nearest palette entry per used bin.
    std::vector<unsigned char> lut(32768);
    for (int b : bins) {
        int r = (b >> 10 & 31) * 255 / 31, g = (b >> 5 & 31) * 255 / 31, bl = (b & 31) * 255 / 31;
        if (cnt[b] && ncol) r = int(sum[b * 3] / cnt[b]), g = int(sum[b * 3 + 1] / cnt[b]), bl = int(sum[b * 3 + 2] / cnt[b]);
        int best = 0, bestD = 1 << 30;
        for (size_t i = 0; i < ncol; ++i) {
            int dr = r - pal[i * 3], dg = g - pal[i * 3 + 1], db = bl - pal[i * 3 + 2];
            int d = dr * dr + dg * dg + db * db;
            if (d < bestD) bestD = d, best = int(i);
        }
        lut[b] = (unsigned char)best;
    }

    Bytes o{'G', 'I', 'F', '8', '9', 'a'};
    put16(o, w);
    put16(o, h);
    o.insert(o.end(), {0xF7, 0, 0}); // global table, 8-bit colour, 256 entries
    o.insert(o.end(), pal.begin(), pal.end());
    if (loop) {
        const char* ns = "\x21\xFF\x0BNETSCAPE2.0\x03\x01";
        o.insert(o.end(), ns, ns + 16);
        o.insert(o.end(), {0, 0, 0});
    }
    Bytes prev, cur(px);
    for (size_t f = 0; f < frames.size(); ++f) {
        for (size_t i = 0; i < px; ++i) cur[i] = lut[bin15(&frames[f][i * 4])];
        int x0 = 0, y0 = 0, x1 = w - 1, y1 = h - 1; // inclusive changed box
        if (!prev.empty()) {
            x0 = w, y0 = h, x1 = -1, y1 = -1;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    if (cur[size_t(y) * w + x] != prev[size_t(y) * w + x])
                        x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
            if (x1 < 0) x0 = y0 = x1 = y1 = 0; // unchanged: 1x1 frame repeating the old pixel
        }
        o.insert(o.end(), {0x21, 0xF9, 4, 1 << 2}); // GCE, disposal 1 (do not dispose)
        put16(o, std::clamp(delaysCs[f], 0, 65535));
        o.insert(o.end(), {0, 0});
        o.push_back(0x2C);
        put16(o, x0);
        put16(o, y0);
        put16(o, x1 - x0 + 1);
        put16(o, y1 - y0 + 1);
        o.push_back(0);
        Bytes sub;
        for (int y = y0; y <= y1; ++y) sub.insert(sub.end(), &cur[size_t(y) * w + x0], &cur[size_t(y) * w + x1] + 1);
        lzw(o, sub);
        prev.swap(cur);
        cur.resize(px);
    }
    o.push_back(0x3B);

    std::ofstream out(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary);
    out.write(reinterpret_cast<const char*>(o.data()), std::streamsize(o.size()));
    return bool(out.flush());
}
