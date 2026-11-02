#pragma once
// Dev/test hook: OPCAN_SCRIPT=<file> drives the UI with a tiny line script (see devscript.cpp).
// Does nothing unless the env var is set.
#include <string>

namespace devscript {
bool active();
void windowSize(int& w, int& h);  // OPCAN_SIZE=WxH (default 1440x900 when a script runs)
void frame();                     // call right before ImGui::NewFrame: injects this frame's input
std::string takeShot();           // non-empty: save the rendered frame to this png
bool done();                      // script finished (or `quit`)
}
