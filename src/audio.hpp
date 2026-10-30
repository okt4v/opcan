#pragma once
#include <memory>
#include <string>

enum class Sfx { Move, Capture, Check, End, Notify, Count };

// Short UI sounds through one miniaudio engine. Silent no-op when there is no audio device.
class Audio {
public:
    Audio();
    ~Audio();
    // Loads Move/Capture/Check/Checkmate/GenericNotify (.mp3/.ogg/.wav) from dir; missing ones fall back to Move/Notify.
    void load(const std::string& dir);
    void play(Sfx s);
    void setVolume(float v);  // 0..1

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
