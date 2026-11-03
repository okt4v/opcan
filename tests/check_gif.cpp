#include "gif.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#define CHECK(x) \
    do { \
        if (!(x)) { \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
            std::exit(1); \
        } \
    } while (0)

void checkGif() {
    const int w = 64, h = 48;
    std::vector<std::vector<unsigned char>> frames;
    for (int f = 0; f < 3; ++f) {
        std::vector<unsigned char> img(size_t(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                unsigned char* p = &img[(size_t(y) * w + x) * 4];
                bool light = ((x / 8) + (y / 8)) % 2 == 0, sq = x >= 8 + f * 16 && x < 20 + f * 16 && y >= 16 && y < 28;
                p[0] = sq ? 200 : light ? 240 : 118;
                p[1] = sq ? 30 : light ? 217 : 150;
                p[2] = sq ? 30 : light ? 181 : 86;
                p[3] = 255;
            }
        frames.push_back(img);
    }
    frames.push_back(frames.back()); // unchanged frame -> 1x1
    std::vector<int> delays{50, 50, 50, 100};
    std::string path = (std::filesystem::temp_directory_path() / "opcan_check.gif").string();
    CHECK(writeGif(path, w, h, frames, delays));
    std::ifstream in(path, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    CHECK(s.size() > 13 && s.compare(0, 6, "GIF89a") == 0 && (unsigned char)s.back() == 0x3B);
    CHECK((unsigned char)s[6] == w && s[7] == 0 && (unsigned char)s[8] == h && s[9] == 0);
    CHECK(s.find("NETSCAPE2.0") != std::string::npos);
    std::filesystem::remove(path);

    CHECK(!writeGif(path, 0, h, frames, delays));
    CHECK(!writeGif(path, w, h, {}, {}));
    CHECK(!writeGif(path, w, h, frames, {50}));
    CHECK(!writeGif(path, w + 1, h, frames, delays));
}
