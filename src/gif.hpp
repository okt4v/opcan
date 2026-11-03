#pragma once
#include <string>
#include <vector>

// Animated GIF writer (no dependencies). frames: w*h*4 RGBA bytes each (alpha ignored);
// delaysCs: per-frame delay in 1/100 s. loop: repeat forever. False on I/O error or bad input.
// One shared 256-colour palette for the whole animation (board images are mostly flat colours).
bool writeGif(const std::string& path, int w, int h, const std::vector<std::vector<unsigned char>>& frames,
              const std::vector<int>& delaysCs, bool loop = true);
