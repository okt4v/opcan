#include "audio.hpp"
#include <miniaudio.h>
#include <filesystem>
#include <random>

struct Audio::Impl {
    ma_engine engine;
    bool ok = false;
    ma_sound snd[(int)Sfx::Count];
    bool have[(int)Sfx::Count] = {};
    std::string dir;
    std::minstd_rand rng{std::random_device{}()};

    void unload() {
        for (int i = 0; i < (int)Sfx::Count; i++)
            if (have[i]) ma_sound_uninit(&snd[i]), have[i] = false;
    }
};

Audio::Audio() : d(std::make_unique<Impl>()) { d->ok = ma_engine_init(nullptr, &d->engine) == MA_SUCCESS; }

Audio::~Audio() {
    if (!d->ok) return;
    d->unload();
    ma_engine_uninit(&d->engine);
}

void Audio::load(const std::string& dir) {
    if (!d->ok || dir == d->dir) return;
    d->unload();
    d->dir = dir;
    static const char* names[] = {"Move", "Capture", "Check", "Checkmate", "GenericNotify", "Castle", "Success"};
    for (int i = 0; i < (int)Sfx::Count; i++)
        for (const char* ext : {".mp3", ".ogg", ".wav"}) {
            auto p = std::filesystem::path(dir) / (std::string(names[i]) + ext);
            if (!std::filesystem::exists(p)) continue;
            if (ma_sound_init_from_file(&d->engine, p.string().c_str(), MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION,
                                        nullptr, nullptr, &d->snd[i]) == MA_SUCCESS) {
                d->have[i] = true;
                break;
            }
        }
}

void Audio::play(Sfx s) {
    if (!d->ok) return;
    int i = (int)s;
    bool chime = s == Sfx::End || s == Sfx::Notify || s == Sfx::Success;
    if (!d->have[i]) i = chime ? (int)Sfx::Notify : (int)Sfx::Move;
    if (!d->have[i]) return;
    float p = 1, v = 1;
    if (!chime) {
        std::uniform_real_distribution<float> u(-0.03f, 0.03f);
        p += u(d->rng), v += u(d->rng);
    }
    ma_sound_set_pitch(&d->snd[i], p);
    ma_sound_set_volume(&d->snd[i], v);
    ma_sound_seek_to_pcm_frame(&d->snd[i], 0);
    ma_sound_start(&d->snd[i]);
}

void Audio::setVolume(float v) {
    if (d->ok) ma_engine_set_volume(&d->engine, v);
}
