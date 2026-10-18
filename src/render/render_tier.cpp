#include <monkey_dust/render/render_tier.h>
#include <monkey_dust/platform/md_log.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_cpuinfo.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

// ── DetectTier ────────────────────────────────────────────────────────────────
// PCI-ID based classification (CLAUDE_SDL_GPU_PREP_AI.md §I).

RenderTier DetectTier(uint16_t vendor_id, uint16_t device_id, uint32_t vram_mb) {
    if (vendor_id == 0x8086) { // Intel
        if (device_id == 0x1916) return RenderTier::Forward;            // HD 520
        if (device_id == 0x1926 || device_id == 0x1927)
            return RenderTier::Deferred_Low;                             // Iris 540/550
        if (device_id >= 0x5900 && device_id <= 0x5930)
            return RenderTier::Deferred_High;                            // Iris Plus (eDRAM)
        // Other Intel iGPU — heuristic by GFLOPS proxy (EU count unknown)
        return RenderTier::Deferred_Low;
    }
    if (vendor_id == 0x1002) { // AMD
        // Shared-VRAM iGPUs report low vram_mb; Vega 8 ≈ 256-512 MB reserved
        if (vram_mb < 512) return RenderTier::Deferred_Low;  // Vega 8
        return RenderTier::Deferred_Med;                      // Vega 11+
    }
    // NVIDIA / Unknown — assume capable of full pipeline
    return RenderTier::Deferred_High;
}

// ── sysfs PCI ID reader (Linux) ───────────────────────────────────────────────

static bool read_sysfs_ids(uint16_t& vendor, uint16_t& device) {
    // Try card0..card3; skip renderD* symlinks.
    for (int card = 0; card <= 3; ++card) {
        char path_v[64], path_d[64];
        snprintf(path_v, sizeof(path_v), "/sys/class/drm/card%d/device/vendor", card);
        snprintf(path_d, sizeof(path_d), "/sys/class/drm/card%d/device/device", card);

        FILE* fv = fopen(path_v, "r");
        FILE* fd = fopen(path_d, "r");
        if (!fv || !fd) {
            if (fv) fclose(fv);
            if (fd) fclose(fd);
            continue;
        }
        char vbuf[16] = {}, dbuf[16] = {};
        bool ok = fgets(vbuf, sizeof(vbuf), fv) && fgets(dbuf, sizeof(dbuf), fd);
        fclose(fv); fclose(fd);
        if (!ok) continue;

        vendor = static_cast<uint16_t>(strtol(vbuf, nullptr, 16));
        device = static_cast<uint16_t>(strtol(dbuf, nullptr, 16));
        if (vendor) return true;
    }
    return false;
}

// ── Device name string heuristic ─────────────────────────────────────────────

static RenderTier tier_from_name(const char* name) {
    if (!name || !name[0]) return RenderTier::Forward;
    // Intel
    if (strstr(name, "HD 520") || strstr(name, "HD Graphics 520"))
        return RenderTier::Forward;
    if (strstr(name, "Iris Plus") || strstr(name, "Iris(R) Plus"))
        return RenderTier::Deferred_High;
    if (strstr(name, "Iris"))
        return RenderTier::Deferred_Low;
    if (strstr(name, "Intel") || strstr(name, "i915"))
        return RenderTier::Deferred_Low;
    // AMD
    if (strstr(name, "Vega 11") || strstr(name, "RAVEN2"))
        return RenderTier::Deferred_Med;
    if (strstr(name, "Vega") || strstr(name, "Radeon"))
        return RenderTier::Deferred_Low;
    // NVIDIA or unknown
    return RenderTier::Deferred_High;
}

// ── JSON override ─────────────────────────────────────────────────────────────
// {"render_tier": "Forward|Deferred_Low|Deferred_Med|Deferred_High"}

static bool load_json_override(const char* path, RenderTier& out) {
    if (!path) return false;
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char buf[256] = {};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);

    const char* p = strstr(buf, "\"render_tier\"");
    if (!p) return false;
    p += 13;
    while (*p == ':' || *p == ' ' || *p == '"') ++p;

    if (strncmp(p, "Deferred_High", 13) == 0) { out = RenderTier::Deferred_High; return true; }
    if (strncmp(p, "Deferred_Med",  12) == 0) { out = RenderTier::Deferred_Med;  return true; }
    if (strncmp(p, "Deferred_Low",  12) == 0) { out = RenderTier::Deferred_Low;  return true; }
    if (strncmp(p, "Forward",        7) == 0) { out = RenderTier::Forward;        return true; }
    return false;
}

// ── RenderTierSystem ──────────────────────────────────────────────────────────

namespace md {

RenderTierSystem& RenderTierSystem::Get() {
    static RenderTierSystem inst;
    return inst;
}

const char* RenderTierSystem::TierName() const {
    switch (tier_) {
    case RenderTier::Forward:       return "Forward";
    case RenderTier::Deferred_Low:  return "Deferred_Low";
    case RenderTier::Deferred_Med:  return "Deferred_Med";
    case RenderTier::Deferred_High: return "Deferred_High";
    }
    return "Unknown";
}

RenderTier RenderTierSystem::Detect(md::GpuDeviceHandle dev, const char* json_override_path) {
    // Priority 1: JSON override (user-controlled)
    RenderTier override_tier;
    if (load_json_override(json_override_path, override_tier)) {
        tier_ = override_tier;
        MD_LOG(MD_LOG_INFO, "RenderTier: JSON override → %s", TierName());
        return tier_;
    }

    // Priority 2: sysfs PCI IDs (Linux, authoritative)
    uint16_t vendor = 0, device_id = 0;
    if (read_sysfs_ids(vendor, device_id)) {
        uint32_t sys_ram_mb = static_cast<uint32_t>(SDL_GetSystemRAM());
        // iGPU VRAM ≈ system_ram / 4 (rough shared-memory estimate)
        uint32_t vram_mb = sys_ram_mb / 4u;
        tier_ = DetectTier(vendor, device_id, vram_mb);
        MD_LOG(MD_LOG_INFO,
               "RenderTier: PCI %04X:%04X vram≈%uMB → %s",
               vendor, device_id, vram_mb, TierName());
        return tier_;
    }

    // Priority 3: SDL device name string
    if (dev) {
        SDL_PropertiesID props = SDL_GetGPUDeviceProperties(dev);
        const char* name = props ? SDL_GetStringProperty(props, SDL_PROP_GPU_DEVICE_NAME_STRING, "") : "";
        tier_ = tier_from_name(name);
        MD_LOG(MD_LOG_INFO, "RenderTier: device name '%s' → %s", name, TierName());
        return tier_;
    }

    // Fallback: conservative
    tier_ = RenderTier::Forward;
    MD_LOG(MD_LOG_WARNING, "RenderTier: no GPU info available — defaulting to Forward");
    return tier_;
}

} // namespace md

