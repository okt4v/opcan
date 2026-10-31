#pragma once
#include <memory>
#include <string>

enum class Sfx { Move, Capture, Check, End, Notify, Castle, Success, Count };

// Short UI sounds through one miniaudio engine. Silent no-op when there is no audio device.
class Audio {
public:
    Audio();
    ~Audio();
    // Loads Move/Capture/Check/Checkmate/GenericNotify/Castle/Success (.mp3/.ogg/.wav) from dir;
    // missing ones fall back to Move (moves) or GenericNotify (chimes).
    void load(const std::string& dir);
    void play(Sfx s);  // move sounds get a slight random pitch/volume (+-3%) so repeats don't sound canned
    void setVolume(float v);  // 0..1

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
