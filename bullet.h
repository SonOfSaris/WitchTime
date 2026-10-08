// WitchTime: the bullet time after a perfect dodge. Shared with the tests.
//
// The game: a perfect dodge asks heron::timescale_curves_database for the curve "perfect_dodge" (0.6 s; the
// world at 10% -> 1% -> 19% -> 10% of its speed), and everything slows with it, Dylan too.
// The mod: that slow motion plays as the game made it. A second request for the same curve, with the bullet
// time's whole length as its duration, keeps a place in the game's list; the mod writes that entry's value
// itself (the world's speed of the setting, eased back to 1 at the end). Once the game's own slow motion is
// over, the player's clock runs free of the world's:
//   - the player's own time step (coregame::component::LocalTimeScale; animation, movement model, attacks) is
//     multiplied back up in hooks on the systems that write it (witchtime.cpp),
//   - the systems that work on the player and take the game's clock as a parameter (jump, dodge, Rush,
//     input, camera ...: bullet_systems.inc) get a copy of that clock with the player's step in it. The
//     scheduler hands a system its environments as an array of pointers in parameter order; the mod swaps
//     the one pointer while the player's clock runs free and puts the game's back afterwards.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include "player.h"

namespace bullet {
using player::rd;

struct Config { bool on = false; float duration = 4; float world = 0.2f; float player = 1; };
constexpr float kMinDuration = 1.0f;            // shorter than this there is nothing behind the game's own slow motion

// ---- the game's timescale curves
// heron::timescale_curves_database::env::ActiveCurveDatabase:
//   +00 the running curves {pointer, u32 count}   +10 requests   +20 the lowest value of the running curves
//   +24 the last request's id   +28 "none running"
// a running curve (30h): +08 curve resource, +10 the curve once resolved (its length in seconds at +18h),
//   +20 the request's id, +24 duration override (<= 0: the curve's own length; else the curve is stretched
//   over it), +28 seconds run (real time), +2C its value now
// coregame::global::WorldTimeScale: +08 the scale gameplay asks for (what the curves write).
enum { kDbActive = 0x00, kDbActiveCount = 0x08, kDbLowest = 0x20 };
enum { kItemSize = 0x30, kItemCurve = 0x10, kItemId = 0x20, kItemDuration = 0x24, kItemElapsed = 0x28, kItemValue = 0x2c };
enum { kCurveLength = 0x18 };
enum { kWorldScale = 0x08 };
enum { kMaxItems = 64 };

constexpr float kGameLength = 0.6f;             // the perfect dodge curve's length, when the curve cannot be read
constexpr float kBlend = 0.25f;                 // from the game's last value to the setting's
constexpr float kUnslow = 0.2f;                 // the player's clock comes free over this long
constexpr float kOut = 0.35f;                   // back to the world's full speed at the end

inline float smooth(float x) { if (!(x > 0)) return 0; if (x >= 1) return 1; return x * x * (3 - 2 * x); }

// What the bullet time wants at t seconds: the world's speed, how free the player's clock is (0 = with the
// world, 1 = its own) and how much of the bullet time is left in force (1, then down to 0 over the last
// moments). total = its length, game = the length of the game's own slow motion, world = the setting,
// gameEnd = the value the game's curve ended on (< 0: not known).
struct Shape { float scale = 1, unslow = 0, hold = 0; };
inline Shape shape(float t, float total, float game, float world, float gameEnd) {
    Shape s;
    if (!(t >= 0) || !(total > 0) || t >= total) return s;
    if (game > total) game = total;
    float out = (total - game) * 0.5f; if (out > kOut) out = kOut;
    const float endAt = total - out;
    s.hold = (t <= endAt || !(out > 0)) ? 1 : 1 - smooth((t - endAt) / out);
    float w = world;
    if (t >= game && gameEnd >= 0 && gameEnd < world) w = gameEnd + (world - gameEnd) * smooth((t - game) / kBlend);
    s.scale = s.hold >= 1 ? w : 1 - (1 - w) * s.hold;
    s.unslow = t < game ? 0 : smooth((t - game) / kUnslow);
    return s;
}

// The player's speed the bullet time aims at: the setting while it holds, the world's (full) speed at its end.
inline float target(const Config& c, const Shape& s) { return 1 + (c.player - 1) * s.hold; }

// ---- the mod's bullet times that are in the game's list
struct Run { int id = 0, gameId = 0; float gameEnd = -1; bool seen = false; int waited = 0; };
struct Runs {
    enum { kRuns = 4, kWait = 600 };
    Run r[kRuns]; int n = 0;
    void add(int id, int gameId) {
        if (n == kRuns) { for (int i = 1; i < n; i++) r[i - 1] = r[i]; n--; }       // the oldest gives way
        r[n] = Run(); r[n].id = id; r[n].gameId = gameId; n++;
    }
    void drop(int i) { for (int k = i + 1; k < n; k++) r[k - 1] = r[k]; n--; }
};

inline uint8_t* itemOf(const uint8_t* db, int id) {
    uint8_t* items = rd<uint8_t*>(db, kDbActive); const uint32_t n = rd<uint32_t>(db, kDbActiveCount);
    if (!items || n > kMaxItems) return nullptr;
    for (uint32_t i = 0; i < n; i++) if (rd<int>(items + i * kItemSize, kItemId) == id) return items + i * kItemSize;
    return nullptr;
}

struct Now { bool active = false; Shape shape; float t = 0, total = 0, game = 0; bool started = false, ended = false; };

// After the game's update of the curves' values (db = ActiveCurveDatabase, out = WorldTimeScale): the mod's
// entries get the mod's value, and the lowest of all entries is what the world runs at. Returns what the
// newest bullet time says; started / ended = one was seen for the first time / the last one is over.
inline Now after(const Config& c, uint8_t* db, uint8_t* out, Runs& runs) {
    Now now;
    if (!db || !runs.n) return now;
    bool any = false;
    for (int i = 0; i < runs.n;) {
        Run& r = runs.r[i];
        uint8_t* item = itemOf(db, r.id);
        if (!item) {
            if (r.seen || ++r.waited > Runs::kWait) { now.ended = now.ended || r.seen; runs.drop(i); continue; }
            i++; continue;
        }
        if (!r.seen) { r.seen = true; now.started = true; }
        const float t = rd<float>(item, kItemElapsed), total = rd<float>(item, kItemDuration);
        const uint8_t* curve = rd<const uint8_t*>(item, kItemCurve);
        float game = curve ? rd<float>(curve, kCurveLength) : kGameLength;
        if (!(game >= 0.05f && game <= 5)) game = kGameLength;
        if (const uint8_t* own = itemOf(db, r.gameId)) r.gameEnd = rd<float>(own, kItemValue);     // the game's own slow motion still runs
        const Shape s = c.on ? shape(t, total, game, c.world, r.gameEnd) : Shape();           // switched off while one runs: no part in the world's speed
        memcpy(item + kItemValue, &s.scale, 4);
        any = true;
        if (i == runs.n - 1) { now.active = true; now.shape = s; now.t = t; now.total = total; now.game = game; }
        i++;
    }
    if (now.ended && runs.n) now.ended = false;                 // another one still runs
    if (!any) return now;
    const uint8_t* items = rd<const uint8_t*>(db, kDbActive); const uint32_t n = rd<uint32_t>(db, kDbActiveCount);
    float lowest = 1;
    for (uint32_t i = 0; i < n; i++) { const float v = rd<float>(items + i * kItemSize, kItemValue); if (v < lowest) lowest = v; }
    memcpy(db + kDbLowest, &lowest, 4);
    if (out) memcpy(out + kWorldScale, &lowest, 4);
    return now;
}

// ---- the world's clocks
// The world keeps its environments in a sorted table: ids (FNV-1a of the type name) at +585B0h, their number
// at +585B8h, the pointers at +585C0h.
enum : uint32_t { kWorldEnvIds = 0x585b0, kWorldEnvCount = 0x585b8, kWorldEnvs = 0x585c0 };
constexpr uint32_t fnv(const char* s) { uint32_t h = 0x811c9dc5; for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x1000193; return h; }
constexpr uint32_t kFixedTime = fnv("coregame::global::FixedTime");          // 0x4c8470da
constexpr uint32_t kVariableTime = fnv("coregame::global::VariableTime");    // 0x49771fa2
static_assert(kFixedTime == 0x4c8470da && kVariableTime == 0x49771fa2, "environment ids");

inline uint8_t* envOf(const uint8_t* world, uint32_t id) {
    const uint32_t* ids = rd<const uint32_t*>(world, kWorldEnvIds); const uint32_t n = rd<uint32_t>(world, kWorldEnvCount);
    uint8_t* const* envs = rd<uint8_t* const*>(world, kWorldEnvs);
    if (!ids || !envs || !n || n > 0x4000) return nullptr;
    uint32_t lo = 0, hi = n;
    while (lo < hi) { const uint32_t mid = (lo + hi) / 2; if (ids[mid] < id) lo = mid + 1; else hi = mid; }
    return lo < n && ids[lo] == id ? envs[lo] : nullptr;
}

// coregame::global::FixedTime (28h): +00 the step (unscaled step x the world's time scale), +04 the unscaled
// step, +08 1 / step, +10 time (double), +18 ticks, +20 a flag. VariableTime (18h): the same first three and
// the time.
enum { kFixedSize = 0x28, kVariableSize = 0x18, kTimeStep = 0x00, kTimeUnscaled = 0x04, kTimeRate = 0x08 };

// The factor that turns the world's step into the player's: 1 while the player's clock goes with the world;
// with the clock free the player's step = the unscaled step x the speed aimed at, whatever the world is at.
inline float factor(const uint8_t* fixed, float unslow, float aim) {
    if (!(unslow > 0) || !fixed) return 1;
    const float step = rd<float>(fixed, kTimeStep), plain = rd<float>(fixed, kTimeUnscaled);
    if (!(step > 0) || !(plain > 0)) return 1;
    float f = (1 - unslow) + unslow * aim * plain / step;
    if (!(f > 0.01f)) f = 0.01f;
    if (f > 1000) f = 1000;
    return f;
}

// The copies the player's systems read while the player's clock is free.
struct Clocks { alignas(16) uint8_t fixed[kFixedSize] = {}; alignas(16) uint8_t variable[kVariableSize] = {}; };
inline void copyClock(uint8_t* to, const uint8_t* from, size_t size, float f) {
    uint8_t tmp[kFixedSize];
    memcpy(tmp, from, size);
    const float step = rd<float>(from, kTimeStep) * f;
    if (step > 0) { const float rate = 1 / step; memcpy(tmp + kTimeStep, &step, 4); memcpy(tmp + kTimeRate, &rate, 4); }
    memcpy(to, tmp, size);
}
inline void refresh(Clocks& c, const uint8_t* fixed, const uint8_t* variable, float unslow, float aim) {
    if (fixed) copyClock(c.fixed, fixed, kFixedSize, factor(fixed, unslow, aim));
    if (variable) copyClock(c.variable, variable, kVariableSize, factor(variable, unslow, aim));
}

// ---- the scheduler's systems
// One object of 150h per system, all in one array in the order of their ids: +00 id (u16, = its place in the
// array), +08 name ("heron::jump::update_raising::system") and +10 its length, +58 the environments it is
// called with, as pointers in the order of the system function's parameters, +140 / +148 its functions. The
// scheduler calls function(world, id, the object).
// The name is what stands between "getName<void __cdecl " and the first "(" of the function's signature. For
// the game's three template systems (coregame::movementmodel::update_pre_animation::system<class ecs::...)
// that is over 2,000 characters; a name of up to 7 characters lives inside a list that moves when it grows,
// so its pointer may be stale. Neither is a name of the table: such a system is passed over, the walk goes on.
enum { kSysSize = 0x150, kSysId = 0x00, kSysName = 0x08, kSysNameLength = 0x10, kSysEnvs = 0x58 };
enum { kMaxSystems = 8192, kMaxName = 255 };
struct SysTime { const char* name; int fixed, variable; };
constexpr SysTime kSystems[] = {
#include "bullet_systems.inc"
};
constexpr int kSystemCount = (int)(sizeof kSystems / sizeof kSystems[0]);
constexpr int clockCount() { int n = 0; for (const SysTime& s : kSystems) n += (s.fixed >= 0) + (s.variable >= 0); return n; }
constexpr int kClockCount = clockCount();               // the clocks those systems take (some take both)
// With fewer clocks than this in the mod's hands the player's clock is not set free: a player whose attacks
// and animations run at full speed while jump and dodge count the world's slow time would be worse than none.
constexpr int kMinClocks = kClockCount * 3 / 4;

inline const SysTime* known(const char* name, size_t length) {
    for (const SysTime& s : kSystems) if (strlen(s.name) == length && memcmp(s.name, name, length) == 0) return &s;
    return nullptr;
}

struct Swap {
    int systems = 0, named = 0, swapped = 0, restored = 0, already = 0, other = 0, shared = 0;
    int unnamed = 0;                // systems whose name the mod does not read (none, too long, not readable)
    int stale = 0;                  // systems of the plan that are no longer what they were (the game's list was rebuilt)
    int end = 0;                    // why the walk ended: 1 = the next object's id is not its place, 2 = memory that cannot be read, 3 = the most systems the mod walks
    char first[64] = "", last[64] = "";     // the first and the last name read (for the log, when too few were found)
};
inline void keepName(char* to, const char* name, size_t length) { const size_t n = length < 63 ? length : 63; memcpy(to, name, n); to[n] = 0; }

// Where the player's systems are: the walk over the game's list is made once (it takes a while: the game's
// list has 2,500 systems, and the mod asks whether memory may be read before each step), the bullet times
// start from what it found.
struct Plan {
    struct Found { uint8_t* system; uint16_t id; uint8_t** envs; const SysTime* k; bool shared; };
    Found f[kSystemCount]; int n = 0;
    int systems = 0, named = 0, unnamed = 0, shared = 0, other = 0, end = 0;
    char first[64] = "", last[64] = "";
    bool done = false;
};

// The slots that hold one of the mod's copies right now, each with the pointer that was there before. The way
// back goes by this list, not by another walk.
struct Slots {
    enum { kMax = 192 };
    uint8_t** at[kMax]; uint8_t* was[kMax]; int n = 0;
};
using Readable = bool (*)(const void* p, size_t n);        // may this memory be read? (null: it may)

inline void take(uint8_t** slot, const uint8_t* real, uint8_t* copy, Slots& slots, Readable ok, Swap& w) {
    if (!real || (ok && !ok(slot, sizeof *slot))) { w.other++; return; }
    if (*slot == copy) { w.already++; return; }
    if (*slot != real || slots.n == Slots::kMax) { w.other++; return; }
    slots.at[slots.n] = slot; slots.was[slots.n] = *slot; slots.n++;        // noted first: whatever happens next, the way back is known
    *slot = copy; w.swapped++;
}

// Walks the scheduler's systems from any one of them and notes the ones in the table. The walk ends where the
// array does not go on (an object whose id is not its place) or cannot be read. A system whose list of
// environments is also the list of a system outside the table (shared) is noted as such: that one would get
// the player's clock with it, so it is left alone later.
inline void survey(uint8_t* anySystem, Plan& p, Readable ok = nullptr) {
    p = Plan(); p.done = true;
    if (!anySystem || (ok && !ok(anySystem, kSysSize))) return;
    uint8_t* base = anySystem - (size_t)rd<uint16_t>(anySystem, kSysId) * kSysSize;
    p.end = 3;
    for (int i = 0; i < kMaxSystems; i++) {
        uint8_t* s = base + (size_t)i * kSysSize;
        if (ok && !ok(s, kSysSize)) { p.end = 2; break; }
        if (rd<uint16_t>(s, kSysId) != (uint16_t)i) { p.end = 1; break; }
        p.systems++;
        const char* name = rd<const char*>(s, kSysName); const uint64_t length = rd<uint64_t>(s, kSysNameLength);
        if (!name || !length || length > kMaxName || (ok && !ok(name, (size_t)length))) { p.unnamed++; continue; }
        if (!p.first[0]) keepName(p.first, name, (size_t)length);
        keepName(p.last, name, (size_t)length);
        const SysTime* k = known(name, (size_t)length);
        if (!k) continue;
        p.named++;
        uint8_t** envs = rd<uint8_t**>(s, kSysEnvs);
        if (!envs || p.n == kSystemCount) { p.other++; continue; }
        p.f[p.n++] = Plan::Found{s, (uint16_t)i, envs, k, false};
    }
    for (int i = 0, next = 0; p.n && i < p.systems; i++) {
        if (next < p.n && p.f[next].id == (uint16_t)i) { next++; continue; }
        uint8_t** envs = rd<uint8_t**>(base + (size_t)i * kSysSize, kSysEnvs);
        for (int j = 0; envs && j < p.n; j++) if (p.f[j].envs == envs && !p.f[j].shared) { p.f[j].shared = true; p.shared++; }
    }
}

// Hands the systems of the plan the mod's clocks. A slot is only changed when it holds the game's clock; one
// that holds anything else is left alone and counted. A system of the plan still counts when its object has
// the id and the list of environments it had at the survey; otherwise it is stale (the game's list was
// rebuilt) and a new survey is due.
inline Swap engage(const Plan& p, const uint8_t* fixed, const uint8_t* variable, Clocks& c, Slots& slots, Readable ok = nullptr) {
    Swap w;
    w.systems = p.systems; w.unnamed = p.unnamed; w.shared = p.shared; w.other = p.other; w.end = p.end;
    memcpy(w.first, p.first, sizeof w.first); memcpy(w.last, p.last, sizeof w.last);
    for (int j = 0; j < p.n; j++) {
        const Plan::Found& f = p.f[j];
        if (ok && !ok(f.system, kSysSize)) { w.stale++; continue; }
        if (rd<uint16_t>(f.system, kSysId) != f.id || rd<uint8_t**>(f.system, kSysEnvs) != f.envs) { w.stale++; continue; }
        w.named++;
        if (f.shared) continue;
        if (f.k->fixed >= 0) take(f.envs + f.k->fixed, fixed, c.fixed, slots, ok, w);
        if (f.k->variable >= 0) take(f.envs + f.k->variable, variable, c.variable, slots, ok, w);
    }
    w.named += p.other;
    return w;
}
// Both at once, from any system of the array.
inline Swap engage(uint8_t* anySystem, const uint8_t* fixed, const uint8_t* variable, Clocks& c, Slots& slots, Readable ok = nullptr) {
    Plan p; survey(anySystem, p, ok);
    return engage(p, fixed, variable, c, slots, ok);
}

// The way back: every slot the mod changed gets what it held before, if the mod's copy is still in it.
inline int release(Slots& slots, const Clocks& c, Readable ok = nullptr) {
    int restored = 0;
    for (int i = 0; i < slots.n; i++) {
        uint8_t** slot = slots.at[i];
        if (ok && !ok(slot, sizeof *slot)) continue;
        if (*slot == c.fixed || *slot == c.variable) { *slot = slots.was[i]; restored++; }
    }
    slots.n = 0;
    return restored;
}

} // namespace bullet
