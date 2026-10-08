// WitchTime: the player's entity and time step, and the world's tables they are found through, as the game's
// own code reads them. Shared with the tests.
//
// Every character has its own time step (coregame::component::LocalTimeScale = {fixed step, variable step});
// the game writes the frame's step into it every frame (heron::hit_stop::update_timescales; for characters
// with LocalTimeScaleRatios the engine's coregame::local_time_scale::update writes a blend of two clocks) and
// everything that moves the character reads it: animation, root motion, the melee logic, the weapon's
// attachments. The mod multiplies the player's step right after the game has written it.
#pragma once
#include <cstdint>
#include <cstring>

namespace player {

template <class T> inline T rd(const uint8_t* p, size_t off) { T v; memcpy(&v, p + off, sizeof v); return v; }

// Row of heron::melee_attack::on_update (only the player has a combo graph, so its body runs for the player
// alone): one array pointer per component, then the chunk and the entity's index.
enum { kRowTimeStep = 0x12, kRowChunk = 0x14, kRowIndex = 0x15, kRowSize = 22 };
enum { kTimeStepSize = 8 };
// The world (ecs storage): entity tables, and per archetype the chunk, the entity count and the component table.
enum : uint32_t { kWorldChunks = 0x50, kWorldCounts = 0x10448, kWorldTables = 0x18448, kTableSize = 0x20, kTableNames = 8, kTableOffsets = 0x10, kTableCount = 0x1c,
                  kWorldEntityCount = 0x58510, kWorldGenerations = 0x584e8, kWorldLocations = 0x58530, kMaxArchetypes = 0x2000, kChunkEntities = 0x10 };
constexpr uint32_t kTimeStepComponent = 0xd1a7af97;     // id hash of coregame::component::LocalTimeScale in the component tables

// ---- where an entity's components are
// The game's own way from an entity to its components: entity table -> archetype -> chunk -> component offset.
// name = the component's id in the archetype's table (FNV-1a of its type name), size = one entity's worth of it.
// onlyArchetype >= 0: only when the entity is in that archetype. Returns null when anything is not as expected.
inline uint8_t* componentOf(const uint8_t* world, uint64_t entity, int onlyArchetype, uint32_t name, size_t size) {
    uint32_t index = (uint32_t)entity, generation = (uint32_t)(entity >> 32);
    if (index == 0xffffffff || index >= rd<uint64_t>(world, kWorldEntityCount)) return nullptr;
    if ((uint32_t)rd<const uint64_t*>(world, kWorldGenerations)[index] != generation) return nullptr;
    uint16_t arch = (uint16_t)rd<const uint64_t*>(world, kWorldLocations)[index];
    if (arch >= kMaxArchetypes) return nullptr;
    if (onlyArchetype >= 0 && arch != onlyArchetype) return nullptr;
    uint8_t* chunk = rd<uint8_t*>(world, kWorldChunks + (size_t)arch * 8);
    uint32_t count = chunk ? rd<uint32_t>(world, kWorldCounts + (size_t)arch * 4) : 0;
    if (count > 0x100000) return nullptr;
    uint32_t row = 0;
    for (; row < count; row++) if (rd<uint64_t>(chunk, kChunkEntities + (size_t)row * 8) == entity) break;
    if (row >= count) return nullptr;
    const uint8_t* table = world + kWorldTables + (size_t)arch * kTableSize;
    const uint32_t* names = rd<const uint32_t*>(table, kTableNames);
    const uint32_t* offsets = rd<const uint32_t*>(table, kTableOffsets);
    uint32_t n = rd<uint32_t>(table, kTableCount);
    if (!names || !offsets || n > 4096) return nullptr;
    for (uint32_t i = 0; i < n; i++)
        if (names[i] == name) return offsets[i] ? chunk + offsets[i] + (size_t)row * size : nullptr;
    return nullptr;
}
inline float* timeStepOf(const uint8_t* world, uint64_t entity, int onlyArchetype) {
    return (float*)componentOf(world, entity, onlyArchetype, kTimeStepComponent, kTimeStepSize);
}

// ---- the time step
struct Applied { float fixed = -1, variable = -1, factor = 1; };

// Multiplies a time step the game has just written. A value this function wrote itself is left alone, so it
// can run after every place where the game writes without counting twice.
inline bool apply(float* step, float factor, Applied& a) {
    if (factor == 1) return false;
    bool wrote = false;
    if (step[0] != a.fixed) { step[0] *= factor; a.fixed = step[0]; a.factor = factor; wrote = true; }
    if (step[1] != a.variable) { step[1] *= factor; a.variable = step[1]; wrote = true; }
    return wrote;
}
// The factor that is in the fixed step right now (1 when the game's own value is there, as during hit stop).
inline float baked(const float* step, const Applied& a) { return step && step[0] == a.fixed ? a.factor : 1; }

} // namespace player
