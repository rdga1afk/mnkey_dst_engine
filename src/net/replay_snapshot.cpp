#include <monkey_dust/net/replay_snapshot.h>
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/components/agent_state.h>
#include <cstdio>
#include <cstring>

static uint32_t crc32_rply(const void* data, size_t len) noexcept {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

namespace ReplaySerializer {

uint32_t ComputeFrameCRC(const ReplayFrame& frame) noexcept {
    size_t bytes = (size_t)frame.npc_count * sizeof(ReplayNpcState);
    if (bytes == 0 || frame.npc_count > REPLAY_MAX_NPCS_PER_FRAME) return 0u;
    return crc32_rply(frame.npcs, bytes);
}

void CaptureFrame(ReplayBuffer& buf, const ReplayFrame& frame) noexcept {
    buf.frames[buf.write_pos] = frame;
    buf.write_pos = (buf.write_pos + 1u) % (uint32_t)REPLAY_MAX_FRAMES;
    if (buf.count < (uint32_t)REPLAY_MAX_FRAMES) ++buf.count;
}

void CaptureFromECS(ReplayBuffer& buf,
                    float player_x, float player_z,
                    uint32_t frame_index, uint32_t now_ms) noexcept {
    ReplayFrame frame = {};
    frame.frame_index = frame_index;
    frame.timestamp_ms = now_ms;
    frame.player_x     = player_x;
    frame.player_z     = player_z;

    auto& reg = MdRegistry::Get();
    uint16_t n = 0;
#if defined(MD_ECS_GAIA)
    static auto q_p3_replay_snapshot_1 = reg.Raw().query().all<WorldTransform>();
#else
    static auto q_p3_replay_snapshot_1 = reg.Raw().query<WorldTransform>();
#endif
    MdEach(q_p3_replay_snapshot_1, [&](MdEntity e, const WorldTransform& wt) {
        if (n >= (uint16_t)REPLAY_MAX_NPCS_PER_FRAME) return;
        ReplayNpcState& ns = frame.npcs[n];
        ns.x         = wt.x;
        ns.z         = wt.z;
        ns.entity_id = e.ToIntegral();
        const AgentState* as = reg.Handle(e).try_get_mut<AgentState>();
        if (as) {
            ns.motivation = (uint8_t)as->motivation;
            ns.awareness  = (uint8_t)as->awareness;
            ns.alertness  = (uint8_t)as->alertness;
            ns.flags      = (uint8_t)(as->lcflags.bits & 0xFFu);
        }
        ++n;
    });
    frame.npc_count = n;
    frame.npc_crc32 = ComputeFrameCRC(frame);
    CaptureFrame(buf, frame);
}

bool WriteToFile(const ReplayBuffer& buf, const char* path) noexcept {
    if (!path) return false;
    FILE* f = fopen(path, "wb");
    if (!f) return false;

    const uint32_t magic   = REPLAY_MAGIC;
    const uint32_t version = REPLAY_VERSION;
    const uint32_t count   = buf.count;

    fwrite(&magic,   sizeof(magic),   1, f);
    fwrite(&version, sizeof(version), 1, f);
    fwrite(&count,   sizeof(count),   1, f);

    // Write frames oldest-first from the ring buffer.
    const uint32_t max = (uint32_t)REPLAY_MAX_FRAMES;
    const uint32_t start = (buf.count < max)
                         ? 0u
                         : buf.write_pos; // oldest entry when full
    for (uint32_t i = 0; i < buf.count; ++i) {
        uint32_t idx = (start + i) % max;
        fwrite(&buf.frames[idx], sizeof(ReplayFrame), 1, f);
    }

    fclose(f);
    return true;
}

bool ReadFromFile(ReplayBuffer& buf, const char* path) noexcept {
    if (!path) return false;
    FILE* f = fopen(path, "rb");
    if (!f) return false;

    uint32_t magic = 0, version = 0, count = 0;
    if (fread(&magic,   sizeof(magic),   1, f) != 1 || magic   != REPLAY_MAGIC   ||
        fread(&version, sizeof(version), 1, f) != 1 || version != REPLAY_VERSION ||
        fread(&count,   sizeof(count),   1, f) != 1) {
        fclose(f); return false;
    }

    Reset(buf);
    const uint32_t to_load = count < (uint32_t)REPLAY_MAX_FRAMES
                           ? count : (uint32_t)REPLAY_MAX_FRAMES;
    for (uint32_t i = 0; i < to_load; ++i) {
        ReplayFrame frame = {};
        if (fread(&frame, sizeof(frame), 1, f) != 1) break;
        CaptureFrame(buf, frame);
    }

    fclose(f);
    return true;
}

void Reset(ReplayBuffer& buf) noexcept {
    buf.write_pos = 0;
    buf.count     = 0;
}

} // namespace ReplaySerializer
