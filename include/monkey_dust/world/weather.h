#pragma once
// Kenshi weather on the CPU: the data chain (areasmap colour -> BIOME_GROUP -> SEASON -> WEATHER records of the player's install), one running weather per
// biome group, and the sky controller that follows the weather of the camera's zone. Built from our reads of the exe (private notes), not from its code.
// Facts: docs/research/kenshi_specs/weather_data.md. Determinism: the draws come from one process-wide MSVC-CRT-style generator (CrtRand) like the original;
// the draw ORDER differs from the original until the rest of the game logic that calls rand() exists (docs/DEVIATIONS.md, temporary).
#include <monkey_dust/io/fcs_gamedata.h>
#include <monkey_dust/render/kenshi_sky.h>
#include <cstdint>
#include <string>
#include <vector>

namespace md::weather {

// The Microsoft C runtime rand(): seed * 214013 + 2531011, output bits 16..30 (0..32767); srand(1) is the start state.
struct CrtRand {
    uint32_t s = 1;
    void Seed(uint32_t v) { s = v; }
    int Next() { s = s * 214013u + 2531011u; return (int)((s >> 16) & 0x7fff); }
    float Unit() { return (float)((double)Next() * (1.0 / 32768.0)); }                         // the exe's float draw: rand() / 32768
    float Range(float lo, float hi) { return Unit() * (hi - lo) + lo; }
    int RangeInt(int a, int b) { int v = a + (int)(Unit() * (float)((b - a) + 1)); return v < a ? a : (v > b ? b : v); }   // a + rand/32768 * (b - a + 1), clamped into [a, b]
};

constexpr int kZones = 64;               // zones per side
constexpr float kZoneUnits = 4608.f;     // 0.1 m units (460.8 m)
constexpr float kZoneOrigin = 147466.25f;   // the world's min corner is -kZoneOrigin
constexpr float kBlendUnits = 1300.f;
constexpr float kTransitionSeconds = 30.f;

struct WeatherDef {
    std::string id;
    float weight = 0.f;                  // 1st value of the season's reference triple
    int dur_min = 0, dur_max = 0;        // 2nd / 3rd value: game minutes
    float start_hour = 0.f, end_hour = 0.f;
    float clouds = 0.f, rain = 0.f, wind = 0.f, wetness = 0.f, dust = 0.f, haze = 0.f;
    float strength_min = 1.f, strength_max = 1.f;
    KsVec3 sky_mult = { 1.f, 1.f, 1.f };
    bool fog_on = false; float fog_min = 0.f, fog_max = 0.f;
};
struct SeasonDef {
    std::string id;
    float share = 1.f;                   // the 2nd triple value, or the equal share
    int days = 1;                        // the group's year (days per year) shared out by the 2nd values of the reference triples
    float limit_min = 0.f, limit_max = 1.f;   // weather strength limit min / max
    bool windowed = false;               // some weather has start hour != end hour
    std::vector<WeatherDef> weathers;    // last entry = the default weather (weight 0) when windowed or empty
};
struct GroupDef {
    std::string name; uint32_t index = 0;
    float mult_min = 1.f, mult_max = 1.f;   // weather strength multiplier min / max
    std::vector<SeasonDef> seasons;
};
struct Data {
    std::vector<GroupDef> groups;        // groups[0] = the default group "NONE" record
    std::vector<int> zone_group;         // kZones * kZones, index = zx * kZones + zz, into groups
    int FindGroup(const std::string& name) const;
};

// The year (days per year) shared out by the seasons' shares: round(share / sum * year), limited to what is left, the last one walked takes the remainder.
// walk_reverse walks from the last season (the original walks a map keyed by record pointer; see docs/DEVIATIONS.md).
std::vector<int> SplitYear(const std::vector<float>& shares, int days_per_year, bool walk_reverse = false);

// areas: RGB, w * h * 3, top row first (the areasmap as decoded by stb_image); returns false when the install has no weather records.
bool LoadData(const md::fcs::GameDataContainer& g, const unsigned char* areas, int w, int h, Data& out);

// The heat haze constant of the original (exe 1409e8f70): target = record "heat haze" * state strength * clamp(6 * sunDirection.y, 0, 1); the value moves toward the target
// by at most dt / 3 per update and jumps to it when closer than that. haze_scaled = record * strength (SkyOutput.haze).
inline float HeatHazeStep(float current, float haze_scaled, float sun_y, float dt) {
    const float k = 6.f * sun_y, f = k >= 1.f ? 1.f : (k <= 0.f ? 0.f : k);
    const float target = haze_scaled * f, step = dt / 3.f, d = target - current;
    if (target == current) return current;
    if ((d < 0.f ? -d : d) >= step) return current + (target < current ? -step : step);
    return target;
}

// The dust block of the original (exe 1409e8f70, 1409e97e3 - 1409e9893), T = record "dust" * state strength: when T > 0 and above the current value it rises by T * dt * 0.017 and stops at T;
// otherwise, when the current value is above T, it falls by dt * max(0.001, -T) and is not clamped at T. The arithmetic is double between float loads and stores (emulator rows tools/tests/data/exe_weather_dust.tsv).
inline float DustStep(float cur, float target, float dt) {
    if (target > 0.f && target > cur) { const float n = float(double(target * dt) * 0.017 + double(cur)); return n > target ? target : n; }
    if (cur > target) { const double m = -0.001 < double(target) ? -0.001 : double(target); return float(double(dt) * m + double(cur)); }
    return cur;
}

// The wetness block of the original (exe 1409ea410, 1409ea5b3 - 1409ea654), target = record "wetness": below the target the stored value rises by 0.01 * dt; at or above it, it falls only while the target is not above 0 and the value is above 0,
// by 0.005 * dt * (1 - max(2 * target, 0)) (double arithmetic between the float load and store); the stored value is not clamped (it can end below 0; the exe clamps it when it applies it). Emulator rows tools/tests/data/exe_weather_wetness.tsv.
inline float WetnessStep(float cur, float target, float dt) {
    if (cur < target) return float(double(cur) + double(dt) * 0.01);
    if (target <= 0.f && cur > 0.f) return float(double(cur) - double(dt) * 0.005 * (1.0 - (2.0 * double(target) > 0.0 ? 2.0 * double(target) : 0.0)));
    return cur;
}

// The rain and wind values of the original (exe 1409e8f70, 1409e95a3 - 1409e970a). Rain = float32(record "rain intensity" * state strength). Wind = float32(double(float32(record "wind intensity" * wind value)) * 0.01), where the wind value
// is the state field +0x18 of the original (its source is not read: UNKNOWN, so World::Update keeps the raw record value in SkyOutput.wind). The start / stop / value events the original raises on changes are not ported.
// Emulator rows tools/tests/data/exe_weather_rainwind.tsv.
inline float RainStep(float rec_rain, float strength) { return rec_rain * strength; }
inline float WindStep(float rec_wind, float wind_value) { return float(double(rec_wind * wind_value) * 0.01); }

struct SkyOutput {
    float clouds_w = 0.25f;              // the controller's coverage value
    KsVec3 sky_mult = { 1.f, 1.f, 1.f };
    float cloud_offset = -0.8f, darkness = 0.f;
    float rain = 0.f, wind = 0.f, wetness = 0.f, dust = 0.f, haze = 0.f;   // current weather; rain, dust and haze are scaled by the state strength like the original (wind is the raw record value: the original multiplies by a wind speed it draws, not reproduced)
    float ground_wetness = 0.f;          // what the terrain's "wetness" uniform gets (the exe clamps it at 0 when it applies it)
    int group = -1;
};

class World {
public:
    void Init(const Data* d, uint32_t seed);
    // minutes: game minutes since day 0; dt: seconds of the frame; x, z: camera position in units (0.1 m) from the world centre.
    void Update(double minutes, float dt, float x, float z);
    const SkyOutput& Out() const { return out_; }
    uint64_t Checksum() const;           // current weather per group, time left, the controller (determinism check)
    int GroupAt(float x, float z) const;
    const WeatherDef* CurrentWeather(int group) const;
    float Strength(int group) const { return (group >= 0 && group < (int)st_.size()) ? st_[group].strength : 1.f; }
private:
    struct State { int season = -1, season_end_day = 0, weather = -1; double end_minute = 0.0; float strength = 1.f; bool changed = false; };
    void PickWeather(int g, double minutes);
    void StartSeason(int g, int day, double minutes);
    const Data* d_ = nullptr; CrtRand rng_; std::vector<State> st_;
    int cur_group_ = -1; float last_x_ = 0.f, last_z_ = 0.f; bool started_ = false;
    // sky controller
    bool ctl_active_ = false; float ctl_t_ = 0.f, ctl_w_rate_ = 0.f; KsVec3 ctl_c_rate_;
    SkyOutput out_;
};

}   // namespace md::weather
