#pragma once
// AudioSystem — miniaudio integration: one looping ambience stream (the only sound the world plays).
// MINIAUDIO_IMPLEMENTATION lives exclusively in audio_system.cpp.
// All miniaudio types are hidden behind this header (no ma_* exposed).

class AudioSystem {
public:
    static AudioSystem& Get();

    bool Init();
    void Shutdown();
    bool IsReady() const { return ready_; }

    // ── Ambience (streaming, looping) ───────────────────────────────────────
    void PlayAmbience (const char* path, float volume = 0.4f);
    void StopAmbience ();

private:
    AudioSystem() = default;

    bool  ready_          = false;
    bool  ambience_loaded_ = false;
};
