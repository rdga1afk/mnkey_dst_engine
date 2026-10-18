// audio_system.cpp — the ONE translation unit that defines MINIAUDIO_IMPLEMENTATION.
// All other files include <monkey_dust/audio/audio_system.h> and stay miniaudio-free.

#define MINIAUDIO_IMPLEMENTATION
#include "external/miniaudio.h"

#include <monkey_dust/audio/audio_system.h>
#include <monkey_dust/platform/md_log.h>
#include <cstring>

// ── File-static miniaudio storage (BSS, no heap) ─────────────────────────────
static ma_engine s_engine;
static ma_sound  s_ambience;

// ── Singleton ─────────────────────────────────────────────────────────────────

AudioSystem& AudioSystem::Get() {
    static AudioSystem inst;
    return inst;
}

bool AudioSystem::Init() {
    if (ready_) return true;
    ma_result r = ma_engine_init(nullptr, &s_engine);
    if (r != MA_SUCCESS) {
        MD_LOG(MD_LOG_WARNING, "[AudioSystem] ma_engine_init failed: %d", (int)r);
        return false;
    }
    ready_ = true;
    MD_LOG(MD_LOG_INFO, "[AudioSystem] Init OK");
    return true;
}

void AudioSystem::Shutdown() {
    if (!ready_) return;
    StopAmbience();
    ma_engine_uninit(&s_engine);
    ready_ = false;
}

// ── Ambience ──────────────────────────────────────────────────────────────────

void AudioSystem::PlayAmbience(const char* path, float volume) {
    if (!ready_ || !path) return;
    StopAmbience();
    ma_result r = ma_sound_init_from_file(&s_engine, path,
                      MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION,
                      nullptr, nullptr, &s_ambience);
    if (r != MA_SUCCESS) {
        MD_LOG(MD_LOG_WARNING, "[AudioSystem] PlayAmbience '%s' failed: %d", path, (int)r);
        return;
    }
    ma_sound_set_looping(&s_ambience, MA_TRUE);
    ma_sound_set_volume(&s_ambience, volume);
    ma_sound_start(&s_ambience);
    ambience_loaded_ = true;
    MD_LOG(MD_LOG_INFO, "[AudioSystem] Ambience: %s", path);
}

void AudioSystem::StopAmbience() {
    if (!ambience_loaded_) return;
    ma_sound_stop(&s_ambience);
    ma_sound_uninit(&s_ambience);
    ambience_loaded_ = false;
}
