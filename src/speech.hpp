#pragma once
#include <memory>
#include <string>
#include <string_view>

// SAN as words for text-to-speech: "Nxe5+" -> "Knight takes e5, check", "O-O" -> "Castles kingside",
// "exd8=Q#" -> "e takes d8, promotes to queen, checkmate". Annotations (!, ?) are ignored.
std::string spokenSan(std::string_view san);

// Text-to-speech through the OS: speech-dispatcher (spd-say) or espeak-ng/espeak on Linux, `say` on macOS,
// System.Speech via one long-lived PowerShell on Windows. Never blocks the UI; a new say() interrupts the
// previous one. Silent no-op when no backend exists.
class Speech {
public:
    Speech();
    ~Speech();
    void say(const std::string& text);
    bool available() const;  // a backend was found

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
