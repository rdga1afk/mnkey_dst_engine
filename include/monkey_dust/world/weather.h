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
    int days = 1;                        // 2nd value of the group's reference triple
    bool windowed = false;               // some weather has start hour != end hour
    std::vector<WeatherDef> weathers;    // last entry = the default weather (weight 0) when windowed or empty
};
struct GroupDef {
    std::string name; uint32_t index = 0;
    std::vector<SeasonDef> seasons;
};
struct Data {
    std::vector<GroupDef> groups;        // groups[0] = the default group "NONE" record
    std::vector<int> zone_group;         // kZones * kZones, index = zx * kZones + zz, into groups
    int FindGroup(const std::string& name) const;
};

// areas: RGB, w * h * 3, top row first (the areasmap as decoded by stb_image); returns false when the install has no weather records.
bool LoadData(const md::fcs::GameDataContainer& g, const unsigned char* areas, int w, int h, Data& out);

struct SkyOutput {
    float clouds_w = 0.25f;              // the controller's coverage value
    KsVec3 sky_mult = { 1.f, 1.f, 1.f };
    float cloud_offset = -0.8f, darkness = 0.f;
    float rain = 0.f, wind = 0.f, wetness = 0.f, dust = 0.f, haze = 0.f;   // raw values of the current weather (the original also scales them by a state strength, not read)
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
