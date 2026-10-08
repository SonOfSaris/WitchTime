// WitchTime: how the mod finds the game's code (byte patterns, "??" = any byte). Shared with the tests.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace pat {

// ---- the player's time step (player.h). The patterns leave out the first bytes of each function: another mod's hook
// may already sit there, and MinHook can chain behind it.
//
// heron::melee_attack::on_update, the per-entity body:
//   void body(Row* row, World** world, void*, void*, const float* frameStep, void*)
// Row = 22 pointers: one array per component (speed.h names the ones used), the chunk, the entity's index.
//     4C 8B DC  4D 89 4B 20  4D 89 43 18  49 89 53 10  53 56 57     (the 18 bytes left out)
//     41 54 41 55 41 56 41 57   48 81 EC 70 02 00 00   ...   4C 8B E9   mov r13, rcx
const char kMelee[] =
    "41 54 41 55 41 56 41 57 48 81 EC 70 02 00 00 C4 C1 78 29 73 B8 C4 C1 78 29 7B A8 C4 41 78 29 43 98 4C 8B E9 "
    "4D 8D 93 C8 FE FF FF C5 FC 10 01 C4 C1 7C 11 02 C5 FC 10 41 20 C4 C1 7C 11 42 20";
const unsigned kMeleeAt = 0x12;
// Inside it, the game looks an entity up in the world's tables; the mod reads the same tables:
//     48 8B 82 10 85 05 00       mov rax, [rdx+58510h]      ; number of entities
//     48 8B 82 E8 84 05 00  48 8B 04 C8   ...  41 3B C0     ; generation of the index == upper half of the handle
//     48 8B 82 30 85 05 00  48 8B 14 C8   ...  66 41 3B D2  ; location: the low 16 bits are the archetype, FFFF = none
const char kEntityLookup[] =
    "48 8B 82 10 85 05 00 90 48 3B C8 73 ?? 48 8B 82 E8 84 05 00 48 8B 04 C8 90 41 3B C0 75 ?? 83 F9 FF 74 ?? "
    "48 8B 82 58 85 05 00 90 48 8B 82 30 85 05 00 48 8B 14 C8 90 66 41 3B D2";
const unsigned kMeleeSpan = 0xD00;          // the lookup has to be within this many bytes of the function's start

// heron::hit_stop::update_timescales writes {fixed step, variable step} into every character's time step.
// Two forms of it exist, the scheduler picks: one that takes one archetype (the store is inlined) and one that
// walks all archetypes (calls a small body per entity).
//   void perArchetype(World** world, uint16_t archetype, Context* ctx)
//     ... 4D 8B 74 D9 50            mov r14, [r9+rbx*8+50h]        ; the archetype's chunk
//     ... 41 8B 84 99 48 04 01 00   mov eax, [r9+rbx*4+10448h]     ; its entity count
//     ... 48 8D 8B 22 0C 00 00  48 C1 E1 05                        ; its component table: world+8 + (archetype+C22h)*20h
//     ... 41 B8 97 AF A7 D1         mov r8d, D1A7AF97h             ; LocalTimeScale
//     ... 4A 8B 84 23 50 84 01 00   8B 1C 88                       ; the component's offset in the chunk
//     ... at +E1h: 8B 4D 00  89 08  41 8B 0F  89 48 04             ; step[0] = fixed, step[1] = variable
const char kStepOne[] =
    "89 7C 24 20 41 54 41 56 41 57 48 83 EC 60 4C 8B 09 0F B7 DA 4D 8B 74 D9 50 33 FF 4D 85 F6 74 0A 41 8B 84 99 48 04 01 00 "
    "EB 02 8B C7 8B F0 85 C0 0F 84 ?? ?? ?? ?? 49 8B 40 58 4C 8B 78 10 48 8B 68 08 4D 8D 61 08 48 8D 8B 22 0C 00 00 48 C1 E1 05 "
    "42 8B 54 21 1C 41 B8 97 AF A7 D1 4A 8B 4C 21 08 E8 ?? ?? ?? ?? 48 63 C8 48 83 F9 FF 74 11 48 C1 E3 05 4A 8B 84 23 50 84 01 00 "
    "8B 1C 88";
const unsigned kStepOneAt = 0x10;
const unsigned char kStepOneStore[] = {0x8B, 0x4D, 0x00, 0x89, 0x08, 0x41, 0x8B, 0x0F, 0x89, 0x48, 0x04};
const unsigned kStepOneStoreAt = 0xE1;
//   void allArchetypes(World** world, void*, Context* ctx)
//     at +10h: 48 83 EC 60  4D 8B C8  48 8B 01  45 0F B7 A8 C8 00 00 00 ...
//     at +142h: the same component table lookup, then the call of the per-entity body
const char kStepAll[] =
    "48 8D 8B 22 0C 00 00 48 C1 E1 05 8B 54 29 1C 41 B8 97 AF A7 D1 48 8B 4C 29 08 E8 ?? ?? ?? ?? 48 63 C8 48 83 F9 FF 74 19 "
    "48 C1 E3 05 48 8B 84 2B 50 84 01 00 8B 1C 88";
const unsigned kStepAllAt = 0x142;
const unsigned char kStepAllStart[] = {0x48, 0x83, 0xEC, 0x60, 0x4D, 0x8B, 0xC8, 0x48, 0x8B, 0x01, 0x45, 0x0F, 0xB7, 0xA8, 0xC8, 0x00, 0x00, 0x00};
const unsigned kStepAllStartAt = 0x10;

// coregame::local_time_scale::update, the engine's own writer of a character's time step (characters with
// LocalTimeScaleRatios: the step is a blend of two clocks). A fixed and a variable system, each again in an
// all-archetypes and a one-archetype form; the two systems are the same code but for the body they call.
//   all archetypes, at +13Ah: the lookups of LocalTimeScale (D1A7AF97h) and LocalTimeScaleRatios (1E080F43h),
//   at +1CCh the call of the per-entity body. Matches twice: fixed and variable.
const char kLocalAll[] =
    "41 B8 97 AF A7 D1 49 8B 4F 10 E8 ?? ?? ?? ?? 48 63 C8 48 83 F9 FF 74 09 49 8B 47 18 8B 3C 88 EB 02 33 FF 41 8B 57 24 "
    "41 B8 43 0F 08 1E 49 8B 4F 10 E8";
const unsigned kLocalAllAt = 0x13A;
const unsigned char kLocalAllStart[] = {0x48, 0x83, 0xEC, 0x50, 0x4D, 0x8B, 0xD8, 0x48, 0x8B, 0x01, 0x45, 0x0F, 0xB7, 0x80, 0xC8, 0x00, 0x00, 0x00};
const unsigned kLocalAllStartAt = 0x10;
const unsigned kLocalAllBodyCall = 0x1CC;
//   one archetype: void (World** world, uint16_t archetype, Context* ctx); at +ACh the call of the same body.
//   The shape is shared with other systems; the one that calls the same body is the one.
const char kLocalOne[] =
    "4D 8B 44 C2 50 4D 85 C0 74 0A 41 8B 84 82 48 04 01 00 EB 02 8B C3 44 8B C8 85 C0 0F 84 ?? ?? ?? ?? 49 8B 43 58 48 8D 4C 24 30 "
    "4C 89 74 24 68 4C 89 7C 24 60 4C 89 54 24 20 4C 8B 78 08 E8 ?? ?? ?? ?? 4C 8B 74 24 48 4D 85 F6 74 74";
const unsigned kLocalOneAt = 0x11;
const unsigned kLocalOneBodyCall = 0xAC;

// ---- bullet time (bullet.h)
// heron::timescale_curves_database: the one function that asks for a timescale curve from C++ (the perfect
// dodge calls it, nothing else does):
//   int startCurve(ActiveCurveDatabase* db, uint32_t curve, float duration)        returns the request's id
//     40 53  48 83 EC 40                                    (the 6 bytes left out)
//     8B 59 24  FF C3                                       mov ebx, [rcx+24h] / inc ebx      ; the id
//     89 54 24 20  89 59 24  48 8D 54 24 20  48 83 C1 10    ; the request list is at db+10h
//     C7 44 24 24 FF FF FF FF  C5 FA 11 54 24 2C  C6 44 24 30 00  89 5C 24 28  E8    ; {curve, -1, id, duration, 0}
const char kStartCurve[] =
    "8B 59 24 FF C3 89 54 24 20 89 59 24 48 8D 54 24 20 48 83 C1 10 C7 44 24 24 FF FF FF FF C5 FA 11 54 24 2C "
    "C6 44 24 30 00 89 5C 24 28 E8";
const unsigned kStartCurveAt = 0x6;
// heron::timescale_curves_database::update_timescales, the body:
//   void update(ActiveCurveDatabase* db, WorldTimeScale* out)
//     48 89 5C 24 10  55 56 57 41 56 41 57  48 83 EC 70     (the 16 bytes left out)
//     ...  4C 8B FA  4C 8B F1  C7 41 20 00 00 80 3F         ; db+20h = 1
//     ...  48 8D 3C 40  48 C1 E7 04  48 03 FB               ; 30h per running curve
// Inside it: a curve's seconds run (+28h) and its duration (+24h) are loaded, its value is stored (+2Ch) and
// goes into the minimum at db+20h, and the minimum is stored as the world's scale (out+8).
const char kTimescales[] =
    "C5 F8 29 74 24 60 C5 F8 29 7C 24 50 C5 78 29 44 24 40 C5 78 29 4C 24 30 C5 78 29 54 24 20 4C 8B FA 4C 8B F1 "
    "C7 41 20 00 00 80 3F 0F B6 49 28 41 C6 46 28 00 49 8B 1E 41 8B 46 08 48 8D 3C 40 48 C1 E7 04 48 03 FB";
const unsigned kTimescalesAt = 0x10;
const char kTimescalesLoads[] = "48 8D 73 1C C4 41 30 57 C9 C5 FA 10 76 0C C5 FA 10 46 08 C4 C1 78 2F C1";
const char kTimescalesStore[] =
    "C5 FA 11 46 10 C4 C1 7A 10 56 20 C5 F8 28 D8 C5 F8 57 C0 C5 FA 10 CB C5 EA 5D E1 C4 C1 7A 11 66 20 "
    "48 83 C3 30 48 83 C6 30 48 3B DF";
const char kTimescalesOut[] = "84 C9 74 06 C4 41 7A 11 57 08";
const unsigned kTimescalesSpan = 0x240;
// What the bullet time relies on outside its two hooks; each is in the game's code exactly once:
// - the scheduler calls a system: 150h per system object, the function at +140h, the object as third
//   argument, its id (the word at +0) as second
//     49 69 D0 50 01 00 00  49 03 52 10  48 8B 82 40 01 00 00  4C 8B C2  0F B7 12
const char kSchedulerCall[] = "49 69 D0 50 01 00 00 49 03 52 10 48 8B 82 40 01 00 00 4C 8B C2 0F B7 12";
// - a system object gets its name: pointer at +8, length at +10h
const char kSystemName[] = "8B 40 04 4D 89 46 08 49 89 46 10 49 8B C6";
// - the world's table of environments (here the script function that reads the world's time scale looks one
//   up): number at +585B8h, sorted ids at +585B0h, pointers at +585C0h
const char kWorldEnvs[] =
    "41 B8 9E 3B 00 7D 8B BB B8 85 05 00 48 8B AB B0 85 05 00 8B D7 48 8B CD E8 ?? ?? ?? ?? 83 F8 FF 0F 84 ?? ?? ?? ?? "
    "48 63 C8 48 8B 83 C0 85 05 00";
// - the engine's clock update: FixedTime +4 = the unscaled step, +0 = that x the time scale, +8 = 1 / step
const char kClockUpdate[] =
    "C5 FA 10 48 14 C5 FA 10 28 48 8B 44 24 28 C5 FA 10 50 04 C5 FA 11 49 04 C5 F2 59 C2 C5 FA 11 01 C5 DA 5E C0 "
    "C5 FA 11 41 08";

inline bool parse(const char* s, std::string& bytes, std::string& mask) {
    bytes.clear(); mask.clear();
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (s[0] == '?') { bytes.push_back(0); mask.push_back('?'); s += (s[1] == '?') ? 2 : 1; continue; }
        unsigned v; if (sscanf_s(s, "%2x", &v) != 1) return false;
        bytes.push_back((char)v); mask.push_back('x'); s += 2;
    }
    return !bytes.empty();
}

// Offset of the first match in [base, base+size), and how many matches there are (counting stops at 2).
inline size_t find(const uint8_t* base, size_t size, const char* pattern, int& count) {
    std::string bytes, mask; count = 0;
    if (!parse(pattern, bytes, mask)) return 0;
    size_t first = 0, n = bytes.size();
    for (size_t i = 0; i + n <= size; i++) {
        if (base[i] != (uint8_t)bytes[0]) continue;
        size_t k = 1;
        for (; k < n; k++) if (mask[k] == 'x' && base[i + k] != (uint8_t)bytes[k]) break;
        if (k == n) { if (!count) first = i; if (++count > 1) break; }
    }
    return first;
}

// Offsets of up to max matches; returns how many there are (counting stops at max + 1).
inline int findAll(const uint8_t* base, size_t size, const char* pattern, size_t* out, int max) {
    std::string bytes, mask; int count = 0;
    if (!parse(pattern, bytes, mask)) return 0;
    size_t n = bytes.size();
    for (size_t i = 0; i + n <= size; i++) {
        if (base[i] != (uint8_t)bytes[0]) continue;
        size_t k = 1;
        for (; k < n; k++) if (mask[k] == 'x' && base[i + k] != (uint8_t)bytes[k]) break;
        if (k == n) { if (count < max) out[count] = i; if (++count > max) break; }
    }
    return count;
}

inline const uint8_t* callTarget(const uint8_t* at) {       // at = an E8 rel32
    return at + 5 + *(const int32_t*)(at + 1);
}

// The functions the player's time step hooks go on, as the mod hooks them. why = what was not found.
struct StepCode {
    const uint8_t* melee = nullptr;
    const uint8_t* stepOne = nullptr; const uint8_t* stepAll = nullptr;         // heron::hit_stop::update_timescales
    const uint8_t* localAll[2] = {}; const uint8_t* localOne[2] = {};           // coregame::local_time_scale::update, fixed and variable
};
inline bool findSteps(const uint8_t* text, size_t size, StepCode& c, const char*& why) {
    int count = 0;
    size_t m = find(text, size, kMelee, count);
    if (count != 1 || m < kMeleeAt) { why = "the melee update"; return false; }
    c.melee = text + m - kMeleeAt;
    size_t span = (size_t)(text + size - c.melee) < kMeleeSpan ? (size_t)(text + size - c.melee) : kMeleeSpan;
    find(c.melee, span, kEntityLookup, count);
    if (count < 1) { why = "the entity lookup inside the melee update"; return false; }
    size_t o = find(text, size, kStepOne, count);
    if (count != 1 || o < kStepOneAt || o - kStepOneAt + kStepOneStoreAt + sizeof kStepOneStore > size) { why = "the time step update (one archetype)"; return false; }
    c.stepOne = text + o - kStepOneAt;
    if (memcmp(c.stepOne + kStepOneStoreAt, kStepOneStore, sizeof kStepOneStore) != 0) { why = "the store in the time step update"; return false; }
    size_t a = find(text, size, kStepAll, count);
    if (count != 1 || a < kStepAllAt) { why = "the time step update (all archetypes)"; return false; }
    c.stepAll = text + a - kStepAllAt;
    if (memcmp(c.stepAll + kStepAllStartAt, kStepAllStart, sizeof kStepAllStart) != 0) { why = "the start of the time step update (all archetypes)"; return false; }
    size_t all[2];
    if (findAll(text, size, kLocalAll, all, 2) != 2 || all[0] < kLocalAllAt) { why = "the engine's time step update (all archetypes, expected twice)"; return false; }
    size_t one[64];
    int nOne = findAll(text, size, kLocalOne, one, 64);
    if (nOne > 64) nOne = 64;
    for (int i = 0; i < 2; i++) {
        const uint8_t* f = text + all[i] - kLocalAllAt;
        if (all[i] - kLocalAllAt + kLocalAllBodyCall + 5 > size || memcmp(f + kLocalAllStartAt, kLocalAllStart, sizeof kLocalAllStart) != 0 || f[kLocalAllBodyCall] != 0xE8) {
            why = "the layout of the engine's time step update"; return false;
        }
        c.localAll[i] = f;
        const uint8_t* body = callTarget(f + kLocalAllBodyCall);
        int found = 0;
        for (int k = 0; k < nOne; k++) {
            if (one[k] < kLocalOneAt || one[k] - kLocalOneAt + kLocalOneBodyCall + 5 > size) continue;
            const uint8_t* g = text + one[k] - kLocalOneAt;
            if (g[kLocalOneBodyCall] == 0xE8 && callTarget(g + kLocalOneBodyCall) == body) { c.localOne[i] = g; found++; }
        }
        if (found != 1) { why = "the engine's time step update (one archetype)"; return false; }
    }
    if (c.localAll[0] == c.localAll[1] || c.localOne[0] == c.localOne[1]) { why = "the engine's two time step updates are the same"; return false; }
    return true;
}

// The engine's clock goes on by one tick: void tick(FixedTime* t)   (ticks +18h, time +10h; the whole function)
//     80 79 20 00  48 8D 41 04                              (the 8 bytes left out)
//     48 0F 44 C1  C5 FA 10 00  48 FF 41 18  C5 FA 5A C0  C5 FB 58 49 10  C5 FB 11 49 10  C3
const char kClockTick[] = "48 0F 44 C1 C5 FA 10 00 48 FF 41 18 C5 FA 5A C0 C5 FB 58 49 10 C5 FB 11 49 10 C3";
const unsigned kClockTickAt = 0x8;

// The functions of the witch time; null when not found exactly once or when the layout the mod relies on is
// not in them. layout = the four things outside the hooks are where the mod expects them. clockTick is a
// refinement the witch time works without.
struct BulletCode { const uint8_t* startCurve = nullptr; const uint8_t* timescales = nullptr; const uint8_t* clockTick = nullptr; bool layout = false; };
inline void findBullet(const uint8_t* text, size_t size, BulletCode& c) {
    int count = 0;
    size_t o = find(text, size, kStartCurve, count);
    if (count == 1 && o >= kStartCurveAt) c.startCurve = text + o - kStartCurveAt;
    o = find(text, size, kTimescales, count);
    if (count == 1 && o >= kTimescalesAt) {
        const uint8_t* f = text + o - kTimescalesAt; const size_t span = (size_t)(text + size - f) < kTimescalesSpan ? (size_t)(text + size - f) : kTimescalesSpan;
        int loads = 0, store = 0; find(f, span, kTimescalesLoads, loads); find(f, span, kTimescalesStore, store);
        find(f, span, kTimescalesOut, count);
        if (count == 1 && loads == 1 && store == 1) c.timescales = f;
    }
    o = find(text, size, kClockTick, count);
    if (count == 1 && o >= kClockTickAt) c.clockTick = text + o - kClockTickAt;
    int a = 0, b = 0, d = 0, e = 0;
    find(text, size, kSchedulerCall, a); find(text, size, kSystemName, b); find(text, size, kWorldEnvs, d); find(text, size, kClockUpdate, e);
    c.layout = a == 1 && b == 1 && d == 1 && e == 1;
}

} // namespace pat
