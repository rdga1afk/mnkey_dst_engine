#include <monkey_dust/world/weather.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace md::weather {

namespace {

float F(const md::fcs::GameData& r, const char* k, float d = 0.f) { auto it = r.floats.find(k); return it == r.floats.end() ? d : it->second; }
int I(const md::fcs::GameData& r, const char* k, int d = 0) { auto it = r.ints.find(k); return it == r.ints.end() ? d : it->second; }
bool B(const md::fcs::GameData& r, const char* k) { auto it = r.bools.find(k); return it != r.bools.end() && it->second; }
KsVec3 Rgb(int argb) { return { (float)((argb >> 16) & 255) / 255.f, (float)((argb >> 8) & 255) / 255.f, (float)(argb & 255) / 255.f }; }

WeatherDef MakeWeather(const md::fcs::GameData& r, const std::array<int32_t, 3>& tri) {
    WeatherDef w;
    w.id = r.string_id;
    w.weight = (float)tri[0]; w.dur_min = tri[1]; w.dur_max = tri[2];
    w.start_hour = F(r, "start time"); w.end_hour = F(r, "end time");
    w.clouds = F(r, "clouds density"); w.rain = F(r, "rain intensity"); w.wind = F(r, "wind intensity"); w.wetness = F(r, "wetness");
    w.dust = F(r, "dust"); w.haze = F(r, "heat haze");
    w.strength_min = F(r, "effect strength min", 1.f); w.strength_max = F(r, "effect strength max", 1.f);
    w.sky_mult = Rgb(I(r, "sky color mult", 0xFFFFFF));
    w.fog_on = B(r, "fog enabled"); w.fog_min = F(r, "fog distance min"); w.fog_max = F(r, "fog distance max");
    return w;
}

float Saturate(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

}   // namespace

int Data::FindGroup(const std::string& name) const {
    for (size_t i = 0; i < groups.size(); ++i) if (groups[i].name == name) return (int)i;
    return -1;
}

bool LoadData(const md::fcs::GameDataContainer& g, const unsigned char* areas, int w, int h, Data& out) {
    out = Data();
    const auto groups = g.OfType(95);
    if (groups.empty() || !areas || w <= 0 || h <= 0) return false;
    constexpr const char* kNone = "18003-gamedata.base";
    for (const md::fcs::GameData* gr : groups) {
        GroupDef gd; gd.name = gr->name; gd.index = (uint32_t)I(*gr, "index") & 0xFFFFFFu;
        if (const auto* refs = gr->Refs("seasons")) {
            for (const auto& ref : *refs) {
                const md::fcs::GameData* s = g.Get(ref.first);
                if (!s) continue;
                SeasonDef sd; sd.id = s->string_id; sd.days = ref.second[1] > 0 ? ref.second[1] : 1;
                if (const auto* wr = s->Refs("weathers")) {
                    for (const auto& wref : *wr) {
                        const md::fcs::GameData* wd = g.Get(wref.first);
                        if (!wd) continue;
                        sd.weathers.push_back(MakeWeather(*wd, wref.second));
                        if (sd.weathers.back().start_hour != sd.weathers.back().end_hour) sd.windowed = true;
                    }
                }
                if (sd.windowed || sd.weathers.empty()) {   // the original appends the default weather record with weight 0 (the fallback of an empty pick)
                    const md::fcs::GameData* def = g.Get("5460-weather.mod");
                    WeatherDef wd = def ? MakeWeather(*def, { 0, 0, 0 }) : WeatherDef();
                    wd.weight = 0.f; sd.weathers.push_back(wd);
                }
                gd.seasons.push_back(std::move(sd));
            }
        }
        if (gr->string_id == kNone) out.groups.insert(out.groups.begin(), std::move(gd)); else out.groups.push_back(std::move(gd));
    }
    out.zone_group.assign(kZones * kZones, 0);
    // zone centre -> areasmap texel: the map covers [-147456, +147456) over w texels
    const float texel_x = 294912.f / (float)w, texel_z = 294912.f / (float)h;
    for (int zx = 0; zx < kZones; ++zx) for (int zz = 0; zz < kZones; ++zz) {
        const float cx = (float)zx * kZoneUnits - kZoneOrigin + 0.5f * kZoneUnits, cz = (float)zz * kZoneUnits - kZoneOrigin + 0.5f * kZoneUnits;
        int col = (int)((cx + 147456.f) / texel_x), row = (int)((cz + 147456.f) / texel_z);
        col = std::min(std::max(col, 0), w - 1); row = std::min(std::max(row, 0), h - 1);
        const unsigned char* p = areas + ((size_t)row * w + col) * 3;
        const uint32_t key = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        for (size_t i = 0; i < out.groups.size(); ++i) if (out.groups[i].index == key) { out.zone_group[zx * kZones + zz] = (int)i; break; }
    }
    return true;
}

void World::Init(const Data* d, uint32_t seed) {
    d_ = d; rng_.Seed(seed); st_.assign(d ? d->groups.size() : 0, State()); cur_group_ = -1; started_ = false;
    ctl_active_ = false; ctl_t_ = 0.f; ctl_w_rate_ = 0.f; ctl_c_rate_ = KsVec3();
    out_ = SkyOutput();
}

int World::GroupAt(float x, float z) const {
    if (!d_) return -1;
    int zx = (int)std::floor((x + kZoneOrigin) / kZoneUnits), zz = (int)std::floor((z + kZoneOrigin) / kZoneUnits);
    zx = std::min(std::max(zx, 0), kZones - 1); zz = std::min(std::max(zz, 0), kZones - 1);
    return d_->zone_group[zx * kZones + zz];
}

const WeatherDef* World::CurrentWeather(int g) const {
    if (!d_ || g < 0 || g >= (int)st_.size() || st_[g].season < 0) return nullptr;
    const SeasonDef& s = d_->groups[g].seasons[st_[g].season];
    return st_[g].weather >= 0 ? &s.weathers[st_[g].weather] : nullptr;
}

// The season's pick: eligible weathers (all, or those whose hour window holds the clock hour), weight walk with one draw; none eligible -> the last entry.
void World::PickWeather(int g, double minutes) {
    State& st = st_[g];
    const SeasonDef& s = d_->groups[g].seasons[st.season];
    const float hour = (float)((int)(minutes / 60.0) % 24);
    float total = 0.f; int only = -1, n = 0;
    std::pair<float, int> walk[64]; int wn = 0;   // fixed array: the pick runs inside the frame update
    for (int i = 0; i < (int)s.weathers.size(); ++i) {
        const WeatherDef& w = s.weathers[i];
        bool ok = !s.windowed || w.start_hour == w.end_hour ||
                  (w.start_hour < w.end_hour ? (w.start_hour <= hour && hour <= w.end_hour) : (hour >= w.start_hour || hour <= w.end_hour));
        if (!ok || !(w.weight > 0.f)) continue;
        if (wn >= 64) continue;
        total += w.weight; walk[wn++] = { total, i }; only = i; ++n;
    }
    int pick = (int)s.weathers.size() - 1;
    if (n == 1) pick = only;
    else if (n > 1) { const float r = rng_.Unit() * total; pick = walk[wn - 1].second; for (int k = 0; k < wn; ++k) if (walk[k].first > r) { pick = walk[k].second; break; } }
    const WeatherDef& w = s.weathers[pick];
    // the setter: strength draw, duration draw (the original also draws two wind-direction values here; not reproduced)
    st.strength = rng_.Range(w.strength_min, w.strength_max);
    const int dur = rng_.RangeInt(w.dur_min, w.dur_max < w.dur_min ? w.dur_min : w.dur_max);
    st.weather = pick; st.changed = true;
    st.end_minute = minutes + (double)dur;
    if (w.start_hour != w.end_hour) {
        const double day_end = (std::floor(minutes / 1440.0) * 24.0 + (double)w.end_hour) * 60.0;
        if (day_end < st.end_minute) st.end_minute = day_end;
    }
}

void World::StartSeason(int g, int day, double minutes) {
    State& st = st_[g];
    const GroupDef& gd = d_->groups[g];
    if (gd.seasons.empty()) { st.season = -1; return; }
    st.season = st.season < 0 ? 0 : (st.season + 1) % (int)gd.seasons.size();
    st.season_end_day = day + gd.seasons[st.season].days;
    PickWeather(g, minutes);
}

void World::Update(double minutes, float dt, float x, float z) {
    if (!d_ || d_->groups.empty()) return;
    const int day = (int)std::floor(minutes / 1440.0);
    for (int g = 0; g < (int)st_.size(); ++g) {
        State& st = st_[g];
        if (d_->groups[g].seasons.empty()) continue;
        if (st.season < 0) { StartSeason(g, day, minutes); continue; }
        if (day >= st.season_end_day) StartSeason(g, day, minutes);
        else if (st.end_minute < minutes) PickWeather(g, minutes);
    }
    // the group of the camera's zone drives the sky
    const int g = GroupAt(x, z);
    const float dx = x - last_x_, dz = z - last_z_;
    const bool jumped = started_ && (dx * dx + dz * dz) > 8000.f;
    bool apply = false;
    if (g != cur_group_) { cur_group_ = g; apply = true; }
    if (g >= 0 && st_[g].changed) apply = true;
    const WeatherDef* w = CurrentWeather(g);
    if (apply && w) {
        ctl_t_ = 0.f; ctl_active_ = true;
        if (jumped) { out_.clouds_w = w->clouds; out_.sky_mult = w->sky_mult; ctl_w_rate_ = 0.f; ctl_c_rate_ = KsVec3(); }
        else {
            ctl_w_rate_ = (w->clouds - out_.clouds_w) / kTransitionSeconds;
            ctl_c_rate_ = { (w->sky_mult.x - out_.sky_mult.x) / kTransitionSeconds, (w->sky_mult.y - out_.sky_mult.y) / kTransitionSeconds, (w->sky_mult.z - out_.sky_mult.z) / kTransitionSeconds };
        }
    }
    for (State& s : st_) s.changed = false;
    if (ctl_active_) {
        ctl_t_ += dt;
        if (ctl_t_ >= kTransitionSeconds) ctl_active_ = false;
        out_.clouds_w = Saturate(out_.clouds_w + dt * ctl_w_rate_);
        out_.sky_mult = { Saturate(out_.sky_mult.x + dt * ctl_c_rate_.x), Saturate(out_.sky_mult.y + dt * ctl_c_rate_.y), Saturate(out_.sky_mult.z + dt * ctl_c_rate_.z) };
    }
    out_.cloud_offset = out_.clouds_w * 1.4f - 0.8f;
    out_.darkness = std::pow(Saturate(out_.clouds_w - 0.5f), 0.3f);
    out_.group = g;
    if (w) { out_.rain = w->rain; out_.wind = w->wind; out_.wetness = w->wetness; out_.dust = w->dust; out_.haze = w->haze; }
    last_x_ = x; last_z_ = z; started_ = true;
}

uint64_t World::Checksum() const {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* p, size_t n) { const unsigned char* b = (const unsigned char*)p; for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; } };
    for (const State& s : st_) { mix(&s.season, 4); mix(&s.season_end_day, 4); mix(&s.weather, 4); mix(&s.end_minute, 8); mix(&s.strength, 4); }
    mix(&out_.clouds_w, 4); mix(&out_.sky_mult, 12); mix(&rng_.s, 4);
    return h;
}

}   // namespace md::weather
