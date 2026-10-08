// Test host for witchtime.dll: an exe whose code holds the game functions the mod looks for.
//   - the melee update: its real first bytes (as long as the mod's pattern), then a tail that notes what it
//     was called with and leaves; the entity lookup the mod wants to see inside it;
//   - the two forms of hit_stop::update_timescales and the engine's two time step systems in both forms,
//     whole, with their calls pointed at stand-ins here;
//   - the request for a timescale curve, whole, its one call pointed at a stand-in; the update of the running
//     curves: its real first bytes, a tail that hands the arguments to a stand-in, and behind it the bytes the
//     mod looks for inside the real function and elsewhere in the game's code; the engine's clock tick, whole.
// A world is built the way the game's storage is laid out, with a player and five other characters, the
// game's list of running timescale curves, the world's clocks and the scheduler's systems. The game of one
// update (the curves count their seconds, the world's time scale follows, the clocks tick, the hit stop system
// writes the time steps, the melee update runs) is played through the DLL's hooks. With /DNO_PATTERN the exe
// has none of the game's code: the mod must leave by itself.
// usage: fakegame.exe <folder with witchtime.dll> [second folder with another copy]
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>
#include "../player.h"
#include "../bullet.h"

static int g_fail = 0, g_checks = 0;
static void check(bool ok, const char* what) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what); g_checks++; if (!ok) g_fail++; }
static const std::string::size_type npos = std::string::npos;

template <class F> static bool waitFor(F cond, DWORD ms) {
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < ms) { if (cond()) return true; Sleep(30); }
    return cond();
}

static std::string readLog(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::string s(1 << 20, 0); DWORD got = 0; ReadFile(h, &s[0], (DWORD)s.size(), &got, nullptr); CloseHandle(h);
    s.resize(got); return s;
}

#ifndef NO_PATTERN
#include "game_bytes.inc"
#pragma section(".patt", read, execute)

// ---- attack speed: the game's melee update (its real first 81 bytes, then a tail that notes what it was
// called with and leaves), the entity lookup the mod wants to see inside it, and the game's two forms of
// hit_stop::update_timescales, whole, with their calls pointed at stand-ins.
extern "C" __declspec(allocate(".patt")) const unsigned char g_meleeFn[] = { MELEE_START_BYTES,
    0x49, 0x8B, 0x43, 0x28,                             // mov  rax, [r11+28h]      ; the frame step (5th argument)
    0xC5, 0xFA, 0x10, 0x00,                             // vmovss xmm0, [rax]
    0x48, 0xBA, 0, 0, 0, 0, 0, 0, 0, 0,                 // mov  rdx, &g_meleeStep   (set at start)
    0xC5, 0xFA, 0x11, 0x02,                             // vmovss [rdx], xmm0
    0x48, 0xBA, 0, 0, 0, 0, 0, 0, 0, 0,                 // mov  rdx, &g_meleeRow
    0x48, 0x89, 0x0A,                                   // mov  [rdx], rcx
    0x48, 0xBA, 0, 0, 0, 0, 0, 0, 0, 0,                 // mov  rdx, &g_meleeCalls
    0xFF, 0x02,                                         // inc  dword [rdx]
    0xC5, 0xF8, 0x77,                                   // vzeroupper
    0xC4, 0xC1, 0x78, 0x28, 0x73, 0xB8,                 // vmovaps xmm6, [r11-48h]
    0xC4, 0xC1, 0x78, 0x28, 0x7B, 0xA8,                 // vmovaps xmm7, [r11-58h]
    0xC4, 0x41, 0x78, 0x28, 0x43, 0x98,                 // vmovaps xmm8, [r11-68h]
    0x48, 0x81, 0xC4, 0x70, 0x02, 0x00, 0x00,           // add  rsp, 270h
    0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C,     // pop  r15 r14 r13 r12
    0x5F, 0x5E, 0x5B,                                   // pop  rdi rsi rbx
    0xC3,
    0xCC, 0xCC, 0xCC,
    ENTITY_LOOKUP_BYTES };
extern "C" __declspec(allocate(".patt")) const unsigned char g_stepOneFn[] = { STEP_ONE_BYTES };
extern "C" __declspec(allocate(".patt")) const unsigned char g_stepAllFn[] = { STEP_ALL_BYTES };
static_assert(sizeof(g_stepOneFn) == kStepOneSize && sizeof(g_stepAllFn) == kStepAllSize, "update_timescales bytes");
// the engine's time step update (coregame::local_time_scale::update), fixed and variable, both forms, whole
extern "C" __declspec(allocate(".patt")) const unsigned char g_localAllFixedFn[] = { LOCAL_ALL_FIXED_BYTES };
extern "C" __declspec(allocate(".patt")) const unsigned char g_localOneFixedFn[] = { LOCAL_ONE_FIXED_BYTES };
extern "C" __declspec(allocate(".patt")) const unsigned char g_localAllVariableFn[] = { LOCAL_ALL_VARIABLE_BYTES };
extern "C" __declspec(allocate(".patt")) const unsigned char g_localOneVariableFn[] = { LOCAL_ONE_VARIABLE_BYTES };
// their per-entity bodies: row = {time steps, ratios, chunk, index}; the step is a blend of two clocks
static void fakeLocalFixed(uint64_t* row, const float* clocks) {
    float r = ((float*)(row[1] + row[3] * 8))[0]; ((float*)(row[0] + row[3] * 8))[0] = (1 - r) * clocks[0] + r * clocks[1];
}
static void fakeLocalVariable(uint64_t* row, const float* clocks) {
    float r = ((float*)(row[1] + row[3] * 8))[1]; ((float*)(row[0] + row[3] * 8))[1] = (1 - r) * clocks[0] + r * clocks[1];
}
static int fakeLookup(const uint32_t* names, uint32_t n, uint32_t id);
// the one-archetype form's helper: where the two components of the archetype are
struct LocalOut { uint64_t x0; uint8_t* chunk; uint64_t x2; uint64_t count; uint8_t* steps; uint8_t* ratios; };
static void fakeLocalResolve(LocalOut* out, uint64_t archetype, uint8_t* chunk, uint32_t count, uint8_t* world) {
    const uint8_t* table = world + 0x18448 + (uint16_t)archetype * 0x20;
    const uint32_t* names = *(const uint32_t**)(table + 8); const uint32_t* offsets = *(const uint32_t**)(table + 0x10); uint32_t n = *(const uint32_t*)(table + 0x1c);
    out->chunk = chunk; out->count = count;
    out->steps = chunk + offsets[fakeLookup(names, n, 0xd1a7af97)]; out->ratios = chunk + offsets[fakeLookup(names, n, 0x1e080f43)];
}

// ---- bullet time: the game's request for a timescale curve, whole, its one call pointed at a stand-in that
// puts the curve into the running list at once; and the update of the running curves: its real first bytes,
// a tail that hands the two arguments to a stand-in and leaves the way the game's function does, and behind
// it the bytes the mod looks for inside the real function and elsewhere in the game's code.
extern "C" __declspec(allocate(".patt")) const unsigned char g_startCurveFn[] = { START_CURVE_BYTES };
static_assert(sizeof(g_startCurveFn) == kStartCurveSize, "timescale curve request bytes");
extern "C" __declspec(allocate(".patt")) const unsigned char g_timescalesFn[] = { TIMESCALES_START_BYTES,
    0x49, 0x8B, 0xCE,                                   // mov  rcx, r14            ; the database
    0x49, 0x8B, 0xD7,                                   // mov  rdx, r15            ; the world's time scale
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                 // mov  rax, fakeTimescales (set at start)
    0xFF, 0xD0,                                         // call rax
    TIMESCALES_END_BYTES,
    0xCC, 0xCC,
    TIMESCALES_LOADS_BYTES, 0xCC, TIMESCALES_STORE_BYTES, 0xCC, TIMESCALES_OUT_BYTES, 0xCC,
    SCHEDULER_CALL_BYTES, 0xCC, SYSTEM_NAME_BYTES, 0xCC, WORLD_ENVS_BYTES, 0xCC, CLOCK_UPDATE_BYTES };
// the engine's clock tick, whole
extern "C" __declspec(allocate(".patt")) const unsigned char g_clockTickFn[] = { CLOCK_TICK_BYTES, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };
// The game's list of running timescale curves, as far as the numbers go: a request becomes a running curve
// (the real game does that on its next update), a running curve's value is the curve at the seconds it has
// run (stretched over its duration when it has one), the lowest value is the world's time scale.
#pragma pack(push, 1)
struct CurveItem { int index; uint32_t pad0; uint64_t resource; const uint8_t* curve; uint32_t generation, lock; int id; float duration, elapsed, value; };
struct CurveRequest { uint32_t curve; int index; int id; float duration; uint8_t end; };
#pragma pack(pop)
static_assert(sizeof(CurveItem) == bullet::kItemSize && offsetof(CurveItem, curve) == bullet::kItemCurve && offsetof(CurveItem, id) == bullet::kItemId &&
              offsetof(CurveItem, duration) == bullet::kItemDuration && offsetof(CurveItem, elapsed) == bullet::kItemElapsed && offsetof(CurveItem, value) == bullet::kItemValue, "running curve layout");
static CurveItem g_curveItems[16];
static uint8_t g_curveDb[0x40], g_curve[0x40], g_worldScale[0x10];
static bool g_breakCurveList = false;
static std::vector<CurveRequest> g_curveRequests; static void* g_curveList = nullptr; static int g_timescaleCalls = 0; static void* g_timescaleArgs[2];
// the game's curve "perfect_dodge", from point to point
static float gameCurve(float t) {
    const float x[5] = {0, 0.15f, 0.3f, 0.45f, 0.6f}, y[5] = {0.1f, 0.01f, 0.19f, 0.1f, 0.1f};
    if (t <= 0) return y[0];
    for (int i = 1; i < 5; i++) if (t <= x[i]) return y[i - 1] + (y[i] - y[i - 1]) * (t - x[i - 1]) / (x[i] - x[i - 1]);
    return y[4];
}
static void fakeQueueCurve(void* list, const CurveRequest* r) {
    g_curveList = list; g_curveRequests.push_back(*r);
    uint32_t n; memcpy(&n, g_curveDb + bullet::kDbActiveCount, 4);
    CurveItem& it = g_curveItems[n]; it = CurveItem{}; it.curve = g_curve; it.id = r->id; it.duration = r->duration; it.value = 1;
    n++; memcpy(g_curveDb + bullet::kDbActiveCount, &n, 4);
}
static void fakeTimescales(uint8_t* db, uint8_t* out) {
    g_timescaleCalls++; g_timescaleArgs[0] = db; g_timescaleArgs[1] = out;
    CurveItem* items; uint32_t n; memcpy(&items, db + bullet::kDbActive, 8); memcpy(&n, db + bullet::kDbActiveCount, 4);
    float lowest = 1;
    for (uint32_t i = 0; i < n; i++) {
        float length; memcpy(&length, items[i].curve + bullet::kCurveLength, 4);
        float t = items[i].elapsed;
        if (items[i].duration > 0) t = t >= items[i].duration ? length : t / items[i].duration * length;
        items[i].value = gameCurve(t);
        if (items[i].value < lowest) lowest = items[i].value;
    }
    memcpy(db + bullet::kDbLowest, &lowest, 4); memcpy(out + bullet::kWorldScale, &lowest, 4);
    if (g_breakCurveList) { const uint8_t* bad = (const uint8_t*)0x10; memcpy(db + bullet::kDbActive, &bad, 8); }       // (for the test of a fault in the mod's part)
}

static volatile float g_meleeStep = -1; static void* volatile g_meleeRow = nullptr; static volatile int g_meleeCalls = 0;
static int fakeLookup(const uint32_t* names, uint32_t n, uint32_t id) { for (uint32_t i = 0; i < n; i++) if (names[i] == id) return (int)i; return -1; }
static float* fakeRowStep(uint64_t* row) { return (float*)(row[0] + row[2] * 8); }
static void fakeStepBody(uint64_t* row, const float* fixedStep, const float* variableStep) { float* s = (float*)(row[0] + row[2] * 8); s[0] = *fixedStep; s[1] = *variableStep; }

static void patch(const unsigned char* at, const void* bytes, size_t n) {
    DWORD old; VirtualProtect((void*)at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)at, bytes, n);
    VirtualProtect((void*)at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
}
static void rel32(const unsigned char* fn, unsigned at, const void* target) {
    int32_t r = (int32_t)((const uint8_t*)target - (fn + at + 5)); patch(fn + at + 1, &r, 4);
}
static void disp32(const unsigned char* fn, unsigned at, const void* target) {      // a rip-relative operand at the end of its instruction
    int32_t r = (int32_t)((const uint8_t*)target - (fn + at + 4)); patch(fn + at, &r, 4);
}
static void link() {
    void* p;
    p = (void*)&g_meleeStep; patch(g_meleeFn + kMeleeStartSize + 10, &p, 8);
    p = (void*)&g_meleeRow; patch(g_meleeFn + kMeleeStartSize + 24, &p, 8);
    p = (void*)&g_meleeCalls; patch(g_meleeFn + kMeleeStartSize + 37, &p, 8);
    rel32(g_stepOneFn, 0x71, (void*)fakeLookup); rel32(g_stepOneFn, 0xdc, (void*)fakeRowStep);
    rel32(g_stepAllFn, 0x15c, (void*)fakeLookup); rel32(g_stepAllFn, 0x1ba, (void*)fakeStepBody);
    for (const unsigned char* f : {g_localAllFixedFn, g_localAllVariableFn}) { rel32(f, 0x144, (void*)fakeLookup); rel32(f, 0x16b, (void*)fakeLookup); }
    rel32(g_localAllFixedFn, 0x1cc, (void*)fakeLocalFixed); rel32(g_localAllVariableFn, 0x1cc, (void*)fakeLocalVariable);
    rel32(g_localOneFixedFn, 0x4e, (void*)fakeLocalResolve); rel32(g_localOneVariableFn, 0x4e, (void*)fakeLocalResolve);
    rel32(g_localOneFixedFn, 0xac, (void*)fakeLocalFixed); rel32(g_localOneVariableFn, 0xac, (void*)fakeLocalVariable);
    rel32(g_startCurveFn, 0x32, (void*)fakeQueueCurve);
    p = (void*)fakeTimescales; patch(g_timescalesFn + kTimescalesStartSize + 8, &p, 8);
}
static const void* code(const unsigned char* p) { const unsigned char* volatile q = p; return q; }
using MeleeFn = void (*)(uint64_t* row, void* world, void* a3, void* a4, float* frameStep, void* a6);
using StepOneFn = void (*)(uint8_t** world, uint64_t archetype, void* ctx);
using StepAllFn = void (*)(uint8_t** world, void* a2, void* ctx);
static MeleeFn meleeFn() { const unsigned char* volatile p = g_meleeFn; return (MeleeFn)(const void*)p; }
static StepOneFn stepOneFn() { const unsigned char* volatile p = g_stepOneFn; return (StepOneFn)(const void*)p; }
static StepAllFn stepAllFn() { const unsigned char* volatile p = g_stepAllFn; return (StepAllFn)(const void*)p; }

// ---- the world of the time step tests: two archetypes of characters with time steps, the player second in
// the first one, laid out as the game's storage is (player.h); and the melee update's row for the player.
namespace sw {
using namespace player;
static uint8_t g_world[0x60000];
static uint8_t* g_worldPtr = g_world;
static uint8_t g_chunk5[0x1000], g_chunk9[0x1000];
static const uint32_t kRatios = 0x1e080f43;         // coregame::component::LocalTimeScaleRatios
static uint32_t g_names5[3] = {0x11111111, kTimeStepComponent, kRatios}, g_offsets5[3] = {0x100, 0x200, 0x300};
static uint32_t g_names9[12], g_offsets9[12];
static uint64_t g_generations[64], g_locations[64];
static const uint64_t kPlayer = (3ull << 32) | 7;
static const uint64_t kOthers[5] = {(1ull << 32) | 2, (9ull << 32) | 5, (2ull << 32) | 11, (4ull << 32) | 12, (1ull << 32) | 13};
static float g_fixed = 1.0f / 60, g_variable = 0.0123f;
static void* g_env[3] = {nullptr, &g_fixed, &g_variable};
static uint8_t g_ctx[0x200];
// the engine's systems get one clock pair each: {another clock, the frame's step}; with ratio 1 the step is the second
static float g_clocksFixed[2] = {0.5f, 1.0f / 60}, g_clocksVariable[2] = {0.5f, 0.0123f};
static void* g_envFixed[2] = {nullptr, g_clocksFixed}; static void* g_envVariable[2] = {nullptr, g_clocksVariable};
static uint8_t g_ctxFixed[0x200], g_ctxVariable[0x200];
static uint64_t g_bits[0x100];
static uint64_t g_archetypeCount[2] = {0, 16};
static uint64_t g_row[kRowSize];

static void putEntity(uint8_t* chunk, int i, uint64_t e, uint16_t arch) {
    memcpy(chunk + kChunkEntities + i * 8, &e, 8); g_generations[(uint32_t)e] = e >> 32; g_locations[(uint32_t)e] = arch;
}
static void archetype(uint16_t id, uint8_t* chunk, uint32_t count, uint32_t* names, uint32_t* offsets, uint32_t n) {
    memcpy(g_world + kWorldChunks + id * 8, &chunk, 8); memcpy(g_world + kWorldCounts + id * 4, &count, 4);
    uint8_t* table = g_world + kWorldTables + id * kTableSize;
    memcpy(table + kTableNames, &names, 8); memcpy(table + kTableOffsets, &offsets, 8); memcpy(table + kTableCount, &n, 4);
}
static float* step5(int i) { return (float*)(g_chunk5 + 0x200 + i * 8); }
static float* step9(int i) { return (float*)(g_chunk9 + 0x80 + 0x40 * 9 + i * 8); }
static void build() {
    for (uint64_t& l : g_locations) l = 0xffff;
    uint64_t n = 64; memcpy(g_world + kWorldEntityCount, &n, 8);
    uint64_t* gp = g_generations; uint64_t* lp = g_locations;
    memcpy(g_world + kWorldGenerations, &gp, 8); memcpy(g_world + kWorldLocations, &lp, 8);
    putEntity(g_chunk5, 0, kOthers[0], 5); putEntity(g_chunk5, 1, kPlayer, 5); putEntity(g_chunk5, 2, kOthers[1], 5);
    for (int i = 0; i < 3; i++) putEntity(g_chunk9, i, kOthers[2 + i], 9);
    for (uint32_t i = 0; i < 12; i++) { g_names9[i] = i == 9 ? kTimeStepComponent : i == 3 ? kRatios : 0x40000000 + i; g_offsets9[i] = 0x80 + 0x40 * i; }
    for (int i = 0; i < 3; i++) for (int k = 0; k < 2; k++) { ((float*)(g_chunk5 + 0x300 + i * 8))[k] = 1; ((float*)(g_chunk9 + 0x80 + 0x40 * 3 + i * 8))[k] = 1; }
    void** ef = g_envFixed; memcpy(g_ctxFixed + 0x58, &ef, 8); void** ev = g_envVariable; memcpy(g_ctxVariable + 0x58, &ev, 8);
    archetype(5, g_chunk5, 3, g_names5, g_offsets5, 3); archetype(9, g_chunk9, 3, g_names9, g_offsets9, 12);
    void** env = g_env; memcpy(g_ctx + 0x58, &env, 8);
    g_bits[0] = (1ull << 5) | (1ull << 9);
    uint64_t* bp = g_bits; memcpy(g_world + 0x58480, &bp, 8);
    uint64_t* ac = g_archetypeCount; memcpy(g_world + 0x58478, &ac, 8);
    memset(g_row, 0, sizeof g_row);
    g_row[kRowTimeStep] = (uint64_t)(g_chunk5 + 0x200); g_row[kRowChunk] = (uint64_t)g_chunk5; g_row[kRowIndex] = 1;
}
// One frame of the game: the time steps are written (by one form of the system or the other, and by the
// engine's own update when asked), then the melee update runs for the player. Returns the frame step the
// melee update was called with.
static bool g_useAll = false, g_useEngine = false;
static void writeSteps(bool hitStop) {
    if (g_useAll) stepAllFn()(&g_worldPtr, nullptr, g_ctx);
    else { stepOneFn()(&g_worldPtr, 9, g_ctx); stepOneFn()(&g_worldPtr, 5, g_ctx); }
    if (g_useEngine) {                                          // the engine's update writes both parts once more
        g_clocksFixed[1] = g_fixed; g_clocksVariable[1] = g_variable;
        if (g_useAll) {
            ((StepAllFn)code(g_localAllFixedFn))(&g_worldPtr, nullptr, g_ctxFixed); ((StepAllFn)code(g_localAllVariableFn))(&g_worldPtr, nullptr, g_ctxVariable);
        } else for (uint64_t a : {5, 9}) {
            ((StepOneFn)code(g_localOneFixedFn))(&g_worldPtr, a, g_ctxFixed); ((StepOneFn)code(g_localOneVariableFn))(&g_worldPtr, a, g_ctxVariable);
        }
    }
    if (hitStop) { step5(1)[0] = g_fixed * 0.05f; step5(1)[1] = g_variable * 0.05f; }      // update_timescales_from_hit_stop
}
static float frame(bool hitStop = false) {
    writeSteps(hitStop);
    float frameStep = g_fixed;
    meleeFn()(g_row, &g_worldPtr, nullptr, nullptr, &frameStep, nullptr);
    return g_meleeStep;
}
static bool about(float a, float b) { return a > b - 1e-6f && a < b + 1e-6f; }
static bool othersPlain() {
    return step5(0)[0] == g_fixed && step5(0)[1] == g_variable && step5(2)[0] == g_fixed && step5(2)[1] == g_variable &&
           step9(0)[0] == g_fixed && step9(1)[1] == g_variable && step9(2)[0] == g_fixed;
}
}  // namespace sw

// ---- the world of the witch time tests: the running timescale curves, the world's clocks in its table of
// environments, and the scheduler's systems: seven the mod does not know (four of them with names it does
// not read: the engine's template system with a name of 2,773 characters, an empty name, no name, a name in
// memory that cannot be read), then every system of the mod's table. The hit stop system (the one that writes
// the characters' time steps, sw) is system 1 of them.
namespace bw {
using namespace bullet;
#pragma pack(push, 1)
struct Fixed { float step, unscaled, rate; uint32_t pad; double time; uint64_t ticks; uint8_t flag, pad2[7]; };
struct Variable { float step, unscaled, rate; uint32_t pad; double time; };
#pragma pack(pop)
static_assert(sizeof(Fixed) == kFixedSize && sizeof(Variable) == kVariableSize, "clock layout");
static Fixed g_fixedTime; static Variable g_variableTime;
static const float kUnscaled = 1.0f / 60, kFrame = 0.0125f;
enum { kExtra = 7, N = kExtra + kSystemCount, kHitStop = 1 };
static const char* const kExtraNames[kExtra] = {"heron::encounter::update_encounter::system", "heron::hit_stop::update_timescales::system", "heron::ai_movement_model::update_animation_step::system",
                                                "?long", "?empty", "?null", "?gone"};
static char g_longName[2774];
static uint8_t g_systems[(N + 1) * kSysSize];
static uint8_t* g_envs[N][16];
static const char* g_names[N];
static int kJump, kCamera, kRotation, kOdd, kDash;
static uint8_t g_other[0x40];
static uint32_t g_envIds[5] = {0x11111111, kVariableTime, kFixedTime, 0x7d003b9e, 0xf0000000};
static uint8_t* g_envPtrs[5] = {g_other, (uint8_t*)&g_variableTime, (uint8_t*)&g_fixedTime, g_worldScale, g_other};
static uint8_t* sys(int i) { return g_systems + i * kSysSize; }
static uint8_t* fixed() { return (uint8_t*)&g_fixedTime; }
static uint8_t* variable() { return (uint8_t*)&g_variableTime; }
static int indexOf(const char* name) { for (int i = 0; i < N; i++) if (strcmp(g_names[i], name) == 0) return i; return -1; }
static void put(uint8_t* s, int id, const char** name, uint8_t** envs) {
    uint16_t i = (uint16_t)id; uint64_t len = strlen(*name);
    memcpy(s + kSysId, &i, 2); memcpy(s + kSysName, name, 8); memcpy(s + kSysNameLength, &len, 8); memcpy(s + kSysEnvs, &envs, 8);
}
static void build() {
    memset(g_systems, 0, sizeof g_systems); memset(g_envs, 0, sizeof g_envs);
    for (int i = 0; i < N; i++) {
        g_names[i] = i < kExtra ? kExtraNames[i] : kSystems[i - kExtra].name;
        put(sys(i), i, &g_names[i], g_envs[i]);
        g_envs[i][1] = i < kExtra ? fixed() : g_other;
        if (i >= kExtra) {
            const SysTime& k = kSystems[i - kExtra];
            if (k.fixed >= 0) g_envs[i][k.fixed] = fixed();
            if (k.variable >= 0) g_envs[i][k.variable] = variable();
        }
    }
    uint16_t none = 0xffff; memcpy(sys(N) + kSysId, &none, 2);         // behind the last system: not one
    {
        memset(g_longName, 'x', 2773); memcpy(g_longName, "coregame::movementmodel::update_pre_animation::system<class ecs::Entity<", 72);
        const char* odd[4] = {g_longName, "", nullptr, (const char*)0x10}; const uint64_t lengths[4] = {2773, 0, 0, 20};
        for (int i = 0; i < 4; i++) { memcpy(sys(3 + i) + kSysName, &odd[i], 8); memcpy(sys(3 + i) + kSysNameLength, &lengths[i], 8); }
    }
    g_envs[kHitStop][2] = variable();
    kJump = indexOf("heron::jump::update_raising::system"); kCamera = indexOf("heron::player_input::update_camera::system");
    kRotation = indexOf("heron::camera_input::calculate_rotation_delta::system"); kOdd = indexOf("heron::dodge::update_dodge::system"); kDash = indexOf("heron::dash::update::system");
    g_envs[kOdd][3] = g_other;                                           // not the clock the mod expects there
    uint32_t* ids = g_envIds; uint8_t** ptrs = g_envPtrs; uint32_t n = 5;
    memcpy(sw::g_world + kWorldEnvIds, &ids, 8); memcpy(sw::g_world + kWorldEnvCount, &n, 4); memcpy(sw::g_world + kWorldEnvs, &ptrs, 8);
    memset(g_curveDb, 0, sizeof g_curveDb); CurveItem* items = g_curveItems; memcpy(g_curveDb + kDbActive, &items, 8);
    float length = 0.6f, one = 1; memcpy(g_curve + kCurveLength, &length, 4); memcpy(g_curveDb + kDbLowest, &one, 4); memcpy(g_worldScale + kWorldScale, &one, 4);
    g_fixedTime = Fixed{kUnscaled, kUnscaled, 60, 0, 100.0, 6000, 0, {}}; g_variableTime = Variable{kFrame, kFrame, 80, 0, 100.0};
}
// A short list of systems, as a game build that names most of them otherwise would have: the hit stop system
// and two the mod knows.
enum { kFew = 4 };
static uint8_t g_few[(kFew + 1) * kSysSize];
static uint8_t* g_fewEnvs[kFew][8];
static const char* g_fewNames[kFew] = {"heron::encounter::update_encounter::system", "heron::hit_stop::update_timescales::system", "heron::jump::update_raising::system", "heron::dash::update::system"};
static void buildFew() {
    memset(g_few, 0, sizeof g_few); memset(g_fewEnvs, 0, sizeof g_fewEnvs);
    for (int i = 0; i < kFew; i++) { put(g_few + i * kSysSize, i, &g_fewNames[i], g_fewEnvs[i]); g_fewEnvs[i][1] = fixed(); }
    g_fewEnvs[1][2] = variable(); g_fewEnvs[2][1] = g_other; g_fewEnvs[2][2] = fixed();
    uint16_t none = 0xffff; memcpy(g_few + kFew * kSysSize + kSysId, &none, 2);
}
// The mod's plan of the list goes stale when a system's list of environments is not the one it was: every
// system of the table gets another one (hide) or its own back.
static void hideList(bool hide) {
    for (int i = kExtra; i < N; i++) { uint8_t** e = hide ? (uint8_t**)g_other : g_envs[i]; memcpy(sys(i) + kSysEnvs, &e, 8); }
}
static uint32_t running() { uint32_t n; memcpy(&n, g_curveDb + kDbActiveCount, 4); return n; }
static float world() { float s; memcpy(&s, g_worldScale + kWorldScale, 4); return s; }
using StartCurveFn = int (*)(uint8_t* db, uint32_t curve, float duration);
using TimescalesFn = void (*)(uint8_t* db, uint8_t* out);
// a perfect dodge: the game asks for its curve (id as in the game's data, the curve's own length)
static int dodge() { return ((StartCurveFn)code(g_startCurveFn))(g_curveDb, 0x5a6f684a, -1.0f); }
struct Tick { float world, player, other, melee, jump, camera, rotationFixed, rotationVariable; bool jumpOwn, cameraOwn, dashOwn, restOwn, fewOwn; float jumpRate; double jumpTime;
              double time, earlyTime; uint64_t ticks, earlyTicks; };          // the game's clock, and what the jump system's clock said right after the engine's tick
static bool g_all = true;
static uint8_t* g_ctx = nullptr;                // the hit stop system the game calls with (null: system 1 of g_systems)
// One update of the game: the running curves count their seconds (real time) and the finished ones go; the
// curves' values make the world's time scale; the engine's clocks follow it; the hit stop system writes the
// characters' time steps; the melee update of the player runs. What a system then reads from its clock.
static Tick tick() {
    uint32_t n = running();
    for (uint32_t i = n; i-- > 0;) {
        CurveItem& it = g_curveItems[i]; it.elapsed += kUnscaled;
        if (it.elapsed >= (it.duration > 0 ? it.duration : 0.6f)) { for (uint32_t k = i + 1; k < n; k++) g_curveItems[k - 1] = g_curveItems[k]; n--; }
    }
    memcpy(g_curveDb + kDbActiveCount, &n, 4);
    ((TimescalesFn)code(g_timescalesFn))(g_curveDb, g_worldScale);
    const float s = world();
    g_fixedTime.step = kUnscaled * s; g_fixedTime.rate = 1 / g_fixedTime.step;
    ((void (*)(uint8_t*))code(g_clockTickFn))((uint8_t*)&g_fixedTime);          // the engine's own tick: time and tick count go on
    g_variableTime.step = kFrame * s; g_variableTime.rate = 1 / g_variableTime.step; g_variableTime.time += g_variableTime.step;
    Tick t{};
    t.time = g_fixedTime.time; t.ticks = g_fixedTime.ticks;
    memcpy(&t.earlyTime, g_envs[kJump][2] + 0x10, 8); memcpy(&t.earlyTicks, g_envs[kJump][2] + 0x18, 8);
    uint8_t* ctx = g_ctx ? g_ctx : sys(kHitStop);
    if (g_all) stepAllFn()(&sw::g_worldPtr, nullptr, ctx);
    else { stepOneFn()(&sw::g_worldPtr, 9, ctx); stepOneFn()(&sw::g_worldPtr, 5, ctx); }
    float frameStep = g_fixedTime.step;
    meleeFn()(sw::g_row, &sw::g_worldPtr, nullptr, nullptr, &frameStep, nullptr);
    t.world = s; t.player = sw::step5(1)[0]; t.other = sw::step5(0)[0]; t.melee = g_meleeStep;
    t.jumpOwn = g_envs[kJump][2] != fixed(); t.cameraOwn = g_envs[kCamera][3] != variable(); t.dashOwn = g_envs[kDash][1] != fixed();
    t.restOwn = g_envs[0][1] != fixed() || g_envs[2][1] != fixed() || g_envs[3][1] != fixed() || g_envs[4][1] != fixed() || g_envs[5][1] != fixed() || g_envs[6][1] != fixed() || g_envs[kHitStop][1] != fixed() || g_envs[kHitStop][2] != variable() || g_envs[kOdd][3] != g_other || g_envs[kJump][1] != g_other;
    t.fewOwn = g_fewEnvs[2][2] != fixed() || g_fewEnvs[3][1] != fixed() || g_fewEnvs[1][1] != fixed() || g_fewEnvs[1][2] != variable();
    memcpy(&t.jump, g_envs[kJump][2] + kTimeStep, 4); memcpy(&t.jumpRate, g_envs[kJump][2] + kTimeRate, 4); memcpy(&t.jumpTime, g_envs[kJump][2] + 0x10, 8);
    memcpy(&t.camera, g_envs[kCamera][3] + kTimeStep, 4);
    memcpy(&t.rotationFixed, g_envs[kRotation][2] + kTimeStep, 4); memcpy(&t.rotationVariable, g_envs[kRotation][1] + kTimeStep, 4);
    return t;
}
static bool nearly(float a, float b, float rel = 1e-4f) { return a > b * (1 - rel) - 1e-9f && a < b * (1 + rel) + 1e-9f; }
// Runs the game until no curve is running any more (and a few updates on); notes every update.
static std::vector<Tick> play(int most = 600) {
    std::vector<Tick> out;
    int idle = 0;
    for (int i = 0; i < most && idle < 5; i++) { out.push_back(tick()); idle = running() ? 0 : idle + 1; }
    return out;
}
}  // namespace bw

#endif

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { printf("usage: fakegame <mod folder> [second folder]\n"); return 2; }
    std::wstring dir = argv[1]; if (dir.back() != L'\\') dir += L'\\';
    std::wstring ini = dir + L"witchtime_config.ini";
    std::wstring dll = dir + L"witchtime.dll";
    std::wstring log = dir + L"witchtime.log";
    SetEnvironmentVariableW(L"WITCHTIME_NO_DIALOGS", L"1");

#ifdef NO_PATTERN
    HMODULE m = LoadLibraryW(dll.c_str());
    check(m != nullptr, "the DLL loads");
    check(waitFor([&] { return GetModuleHandleW(dll.c_str()) == nullptr; }, 4000), "without the game's code the mod unloads itself");
    std::string l = readLog(log);
    check(l.find("not found as expected: the melee update (game build changed?)") != npos && l.find("WitchTime inactive: the game runs unmodified") != npos, "and the log says what was not found");
    check(l.find("message (not shown: dialogs are off):") != npos && l.find("  | WitchTime is switched off for this session.") != npos && l.find("  | Problem: game function \"the melee update\" not found") != npos &&
          l.find("  | Game version: unknown to this mod") != npos && l.find("does not know this game version. A game update has probably changed the code") != npos && l.find("  | To stop this message, set VersionWarning=0") != npos,
          "and has the message it would have shown: the mod is off, the versions, the problem, the likely cause, how to stop the message");
    WritePrivateProfileStringW(L"General", L"VersionWarning", L"0", ini.c_str());
    m = LoadLibraryW(dll.c_str());
    check(m != nullptr && waitFor([&] { return GetModuleHandleW(dll.c_str()) == nullptr; }, 4000) && readLog(log).find("message (not shown: VersionWarning=0):") != npos, "VersionWarning=0: the message is not shown, the log still has it");
#else
    link();
    sw::build();
    // the game's functions run here on their own
    sw::frame();
    check(sw::step5(1)[0] == sw::g_fixed && sw::step5(1)[1] == sw::g_variable && sw::othersPlain() && g_meleeCalls == 1 && g_meleeRow == (void*)sw::g_row && g_meleeStep == sw::g_fixed,
          "without the mod: the hit stop system writes every time step, the melee update runs with its row and the plain frame step");
    bw::build();
    check(bw::dodge() == 1 && g_curveRequests.size() == 1 && g_curveList == g_curveDb + 0x10 && bw::running() == 1, "the game's request for a curve runs here on its own: a perfect dodge puts one curve into the running list");
    {
        std::vector<bw::Tick> r = bw::play();
        check(r.size() >= 36 && r.size() <= 45 && r[5].world < 0.2f && r[5].player == bw::kUnscaled * r[5].world && r.back().world == 1 && bw::running() == 0,
              "and the game's update of the curves runs it out: 0.6 s of slow motion for everyone, then full speed");
    }

    HMODULE m = LoadLibraryW(dll.c_str());
    check(m != nullptr, "the DLL loads");
    if (!m) return 1;
    check(waitFor([&] { return readLog(log).find("active: WitchTime") != npos; }, 5000), "the mod is active");
    std::string l = readLog(log);
    check(l.find("melee update at rva") != npos && l.find("timescale curves: the request at rva") != npos && l.find("the engine's clock tick at rva") != npos && l.find("not found") == npos,
          "it found the melee update, the time step updates, the request for a timescale curve, the curves' update (with the layout it relies on, there and elsewhere) and the clock tick");
    check(l.find("not a build this version knows, trying anyway") != npos, "log: this exe is not a game build the mod knows; it goes on anyway");
    check(l.find("settings: witch time after a perfect dodge: 4 s with the game's own slow motion, then the world at 20% and Dylan at 100% of their speed (0 from the MODS menu)") != npos,
          "log: the settings (on by default: 4 s, the world at 20%, Dylan at 100%)");
    auto* factorX = (volatile float*)(uintptr_t)GetProcAddress(m, "WitchTime_PlayerFactor");
    auto* writtenX = (volatile float*)(uintptr_t)GetProcAddress(m, "WitchTime_Written");
    auto* hookedX = (volatile int*)(uintptr_t)GetProcAddress(m, "WitchTime_Hooked");
    check(factorX && writtenX && hookedX && *hookedX == 1 && *factorX == 1 && *writtenX == 0 && GetProcAddress(m, "WitchTime_InstanceGuard") && GetProcAddress(m, "WitchTime_ThirdPartyNotices"),
          "the exports: WitchTime_Hooked is 1, no factor asked for, nothing written; the instance guard and the notices are there");
    if (!factorX || !writtenX) return 1;

    // ---------------------------------------------------------------- the player's time step
    {
        using namespace sw;
        int mc = g_meleeCalls;
        float seen = sw::frame();
        check(g_meleeCalls == mc + 1 && g_meleeRow == (void*)g_row && seen == g_fixed && step5(1)[0] == g_fixed && step5(1)[1] == g_variable && othersPlain(),
              "nothing running: every time step is the game's, the melee update still gets its row and the plain frame step");
        // another mod's factor on the player's step, outside any witch time
        *factorX = 1.5f;
        check(waitFor([] { sw::frame(); return about(step5(1)[0], g_fixed * 1.5f); }, 2000) && about(step5(1)[1], g_variable * 1.5f) && othersPlain(),
              "WitchTime_PlayerFactor=1.5 from another mod: the player's fixed and variable step are x1.5, nobody else's");
        seen = sw::frame();
        check(about(seen, g_fixed * 1.5f) && *writtenX == step5(1)[0], "the melee update is called with the frame step x1.5 (so the game sees no hit stop), and WitchTime_Written is the step written");
        seen = sw::frame(true);
        check(about(seen, g_fixed) && about(step5(1)[0], g_fixed * 0.05f), "hit stop: the game's slowed step stays, and the melee update gets the plain frame step to see it");
        seen = sw::frame();
        check(about(seen, g_fixed * 1.5f) && about(step5(1)[0], g_fixed * 1.5f), "after the hit stop: x1.5 again");
        g_useAll = true;
        sw::frame(); seen = sw::frame();
        check(about(step5(1)[0], g_fixed * 1.5f) && about(seen, g_fixed * 1.5f) && othersPlain(), "the all-archetypes form of the hit stop system: the same");
        g_variable = 0.02f; sw::frame();
        check(about(step5(1)[1], 0.02f * 1.5f), "a longer frame: the new variable step x1.5");
        g_variable = 0.0123f;
        // the engine's own time step update runs after the hit stop system's and writes both parts again
        g_useEngine = true;
        sw::frame(); seen = sw::frame();
        check(about(step5(1)[0], g_fixed * 1.5f) && about(step5(1)[1], g_variable * 1.5f) && about(seen, g_fixed * 1.5f) && othersPlain(), "with the engine's update in the frame (all archetypes): x1.5 after it too");
        g_useAll = false; sw::frame(); seen = sw::frame();
        check(about(step5(1)[0], g_fixed * 1.5f) && about(step5(1)[1], g_variable * 1.5f) && about(seen, g_fixed * 1.5f) && othersPlain(), "the same in the one-archetype form");
        ((float*)(g_chunk5 + 0x300 + 8))[0] = 0.5f;                  // the player's fixed step is half the other clock
        sw::frame(); seen = sw::frame();
        check(about(step5(1)[0], (0.25f + g_fixed * 0.5f) * 1.5f) && about(seen, g_fixed * 1.5f), "a blended step is multiplied as it is, and the melee update's frame step by the same factor");
        ((float*)(g_chunk5 + 0x300 + 8))[0] = 1;
        g_useEngine = false;
        *factorX = 1; sw::frame(); sw::frame();
        check(step5(1)[0] == g_fixed && step5(1)[1] == g_variable && *writtenX == 0 && g_meleeStep == g_fixed, "the factor back at 1: the game's step, nothing written, the plain frame step");
        *factorX = 0; sw::frame(); sw::frame();
        check(step5(1)[0] == g_fixed, "a factor of 0 (or any nonsense) counts as 1");
        *factorX = 1; sw::frame();
        check(readLog(log).find("the player's time step is found (x1.50 applied)") != npos, "log: the player's time step is found");
    }

    // ---------------------------------------------------------------- the witch time
    {
        using bw::Tick; using bw::nearly; using bw::kUnscaled;
        sw::g_useAll = true;
        l = readLog(log);
        // the mod's walk over the game's list: the first calls of the hit stop system came with the time step
        // tests' stand-in, not a system of the list; the mod tries again every 300th call
        check(l.find("witch time: the player's systems: 0 of the 59 the mod knows are in the game's list of 1 (read in ") != npos, "the first walk, from an object that is not in the game's list, found nothing (and said so)");
        for (int i = 0; i < 300 && readLog(log).find("in the game's list of 66 (read in ") == npos; i++) bw::tick();
        l = readLog(log);
        check(l.find("witch time: the player's systems: 59 of the 59 the mod knows are in the game's list of 66 (read in ") != npos && l.find("(4 without a name the mod reads)") != npos && l.find("read again") == npos,
              "a later try, from the hit stop system, finds all 59 of the player's systems (and passes the four odd names)");
        const int tc = g_timescaleCalls;
        Tick t = bw::tick();
        check(t.world == 1 && t.player == kUnscaled && t.other == kUnscaled && t.melee == kUnscaled && !t.jumpOwn && !t.cameraOwn && !t.restOwn && g_timescaleCalls == tc + 1 &&
              g_timescaleArgs[0] == g_curveDb && g_timescaleArgs[1] == g_worldScale, "no slow motion running: the world and everyone in it at full speed; the game's update of the curves runs with its two arguments");

        // a perfect dodge with the default settings
        size_t before = g_curveRequests.size();
        int id = bw::dodge();
        check(id == 2 && g_curveRequests.size() == before + 2 && g_curveRequests[before].curve == 0x5a6f684a && g_curveRequests[before].duration == -1 && g_curveRequests[before].id == 2 &&
              g_curveRequests[before].index == -1 && g_curveRequests[before].end == 0, "a perfect dodge: the game's own request goes through as it is, and its id comes back");
        check(g_curveRequests[before + 1].curve == 0x5a6f684a && g_curveRequests[before + 1].duration == 4 && g_curveRequests[before + 1].id == 3, "the mod asks for the same curve once more, 4 s long");
        std::vector<Tick> r = bw::play();
        auto at = [&](double s) -> const Tick& { size_t i = (size_t)(s * 60 + 0.5) - 1; return r[i < r.size() ? i : r.size() - 1]; };
        {
            bool game = true, with = true, plain = true;
            for (int i = 0; i < 34; i++) {
                const float want = gameCurve((i + 1) * kUnscaled);
                game = game && nearly(r[i].world, want < 0.2f ? want : 0.2f, 1e-3f);
                with = with && r[i].player == kUnscaled * r[i].world && r[i].other == r[i].player && r[i].melee == r[i].player;
                plain = plain && !r[i].jumpOwn && !r[i].cameraOwn && !r[i].dashOwn && !r[i].restOwn && r[i].jump == r[i].player;
            }
            check(game, "the first 0.6 s are the game's own slow motion: the world at 10% -> 1% -> 19% -> 10%, as the game's curve has it");
            check(with, "Dylan slows with the world for that long, like everyone else (the melee update gets the same step)");
            check(plain, "and every system still reads the game's clock");
        }
        {
            bool rising = true, freeing = true;
            for (int i = 36; i < 52; i++) { rising = rising && r[i].world >= r[i - 1].world - 1e-6f && r[i].world <= 0.2f + 1e-6f; freeing = freeing && r[i].player >= r[i - 1].player - 1e-7f && r[i].player <= kUnscaled * 1.0001f; }
            check(rising && nearly(at(0.9).world, 0.2f), "then the world goes from where the game's curve ended to the 20% of the setting, without a jump");
            check(freeing && nearly(at(0.85).player, kUnscaled), "and Dylan's clock comes free: within 0.2 s the player's time step is the full one again");
        }
        {
            bool hold = true, others = true, melee = true, clocks = true, rest = true;
            for (size_t i = 54; i < 216; i++) {
                hold = hold && nearly(r[i].world, 0.2f, 1e-5f) && nearly(r[i].player, kUnscaled);
                others = others && r[i].other == kUnscaled * r[i].world;
                melee = melee && nearly(r[i].melee, r[i].player, 1e-6f);
                clocks = clocks && r[i].jumpOwn && r[i].cameraOwn && r[i].dashOwn && nearly(r[i].jump, kUnscaled) && nearly(r[i].jumpRate, 60) && nearly(r[i].camera, bw::kFrame) &&
                         nearly(r[i].rotationFixed, kUnscaled) && nearly(r[i].rotationVariable, bw::kFrame) && r[i].jumpTime > 100;
                rest = rest && !r[i].restOwn;
            }
            check(hold, "from there to 3.6 s: the world at 20%, Dylan at full speed");
            check(others, "every other character keeps the world's step");
            check(melee, "the melee update gets the player's step as the frame's (nothing looks like a hit stop)");
            check(clocks, "the player's systems (jump, Rush, camera) read a copy of the game's clocks with the full step in it (and 1 / step, and the game's time)");
            bool exact = true;
            for (size_t i = 54; i < 216; i++) exact = exact && r[i].earlyTime == r[i].time && r[i].earlyTicks == r[i].ticks && r[i].jumpTime == r[i].time && r[i].ticks == r[i - 1].ticks + 1;
            check(exact, "the copy follows the engine's clock tick at once: its time and tick count are the game's before any system runs");
            check(rest, "systems the mod does not know keep the game's clock, and so does a slot that does not hold the clock the mod expects");
        }
        {
            bool easing = true, free = true;
            for (size_t i = 220; i < 239; i++) { easing = easing && r[i].world >= r[i - 1].world - 1e-6f && r[i].world > 0.2f && r[i].world <= 1; free = free && nearly(r[i].player, kUnscaled) && r[i].other == kUnscaled * r[i].world; }
            check(easing && at(3.95).world > 0.9f, "the last 0.35 s the world eases back to its full speed");
            check(free, "Dylan stays at full speed through it");
            const Tick& e = r.back();
            check(r.size() >= 241 && r.size() <= 250 && e.world == 1 && e.player == kUnscaled && e.other == kUnscaled && e.melee == kUnscaled && !e.jumpOwn && !e.cameraOwn && !e.dashOwn && !e.restOwn && e.jump == kUnscaled,
                  "after 4 s it is over: the world at full speed, every system back on the game's clock");
        }
        l = readLog(log);
        check(l.find("witch time: a perfect dodge starts one (timescale curve 0x5a6f684a, requests 2 and 3): 4 s in all") != npos &&
              l.find("witch time: it is in the game's list of running slow motions (4 s; the game's own slow motion takes the first 0.6 s)") != npos, "log: the first perfect dodge and its place in the game's list");
        check(l.find("witch time: the game's own slow motion is over (0.6") != npos && l.find("the world goes on at 20%, the player's clock comes free") != npos &&
              l.find("read again") == npos &&
              l.find("witch time: 59 clocks are now the player's (1 left alone: not the game's clock where the mod expects it)") != npos &&
              l.find("witch time: the player's systems are back on the game's clock (59 clocks)") != npos && l.find("witch time: over, the world is back at its own speed") != npos,
              "log: the clock coming free, the systems that changed clocks (and the one left alone), their way back, the end");

        // the other form of the hit stop system (one archetype at a time), and another mod's factor in the witch time
        bw::g_all = false;
        bw::dodge();
        for (int i = 0; i < 60; i++) t = bw::tick();
        check(nearly(t.world, 0.2f, 1e-5f) && nearly(t.player, kUnscaled) && t.jumpOwn && nearly(t.jump, kUnscaled) && t.other == kUnscaled * t.world, "the same through the one-archetype form of the hit stop system");
        *factorX = 1.5f; bw::tick(); bw::tick(); t = bw::tick();
        check(nearly(t.player, kUnscaled * 1.5f) && nearly(t.melee, t.player, 1e-6f) && t.other == kUnscaled * t.world && nearly(t.jump, kUnscaled),
              "another mod's factor of 1.5 in the witch time: on top of the free clock (x1.5 of the full step); the melee update gets the same step; the systems' clock stays the plain one");
        *factorX = 1;
        r = bw::play();
        check(r.back().world == 1 && r.back().player == kUnscaled && !r.back().jumpOwn, "(that witch time runs out)");
        // the step the mod wrote last is the plain step: a factor right after must still apply
        *factorX = 1.5f; bw::tick(); bw::tick(); t = bw::tick();
        check(nearly(t.player, kUnscaled * 1.5f) && t.world == 1, "a factor right after a witch time still applies (the plain step the mod wrote last is not taken for the mod's own)");
        *factorX = 1; bw::tick(); bw::tick();
        bw::g_all = true;

        // settings
        WritePrivateProfileStringW(L"WitchTime", L"Duration", L"2", ini.c_str());
        WritePrivateProfileStringW(L"WitchTime", L"WorldSpeed", L"50", ini.c_str());
        WritePrivateProfileStringW(L"WitchTime", L"PlayerSpeed", L"150", ini.c_str());
        check(waitFor([&] { bw::tick(); return readLog(log).find("settings: witch time after a perfect dodge: 2 s with the game's own slow motion, then the world at 50% and Dylan at 150% of their speed (0 from the MODS menu)") != npos; }, 4000),
              "[WitchTime] Duration=2, WorldSpeed=50, PlayerSpeed=150 in the ini");
        before = g_curveRequests.size();
        bw::dodge(); r = bw::play();
        check(g_curveRequests.size() == before + 2 && g_curveRequests.back().duration == 2 && r.size() >= 121 && r.size() <= 130, "the mod's request is 2 s long, and after 2 s it is over");
        check(nearly(at(1.2).world, 0.5f, 1e-5f) && nearly(at(1.2).player, kUnscaled * 1.5f) && nearly(at(1.2).jump, kUnscaled * 1.5f) && at(1.2).other == kUnscaled * 0.5f,
              "the world at 50%, Dylan and the player's systems at 150% of the full speed");
        check(at(0.3).world < 0.2f && at(0.3).player == kUnscaled * at(0.3).world && r.back().player == kUnscaled, "the game's own slow motion first, as before; afterwards the plain step");
        CreateDirectoryW((dir + L"ModMenuConfig").c_str(), nullptr);
        std::wstring menu = dir + L"ModMenuConfig\\witchtime.ini";
        WritePrivateProfileStringW(L"Settings", L"duration_ms", L"1500", menu.c_str());
        WritePrivateProfileStringW(L"Settings", L"world_speed", L"30", menu.c_str());
        WritePrivateProfileStringW(L"Settings", L"player_speed", L"100", menu.c_str());
        check(waitFor([&] { bw::tick(); return readLog(log).find("settings: witch time after a perfect dodge: 1.5 s with the game's own slow motion, then the world at 30% and Dylan at 100% of their speed (3 from the MODS menu)") != npos; }, 4000),
              "duration_ms=1500, world_speed=30, player_speed=100 from the MODS menu win over the ini");
        bw::dodge(); r = bw::play();
        check(r.size() >= 91 && r.size() <= 100 && nearly(at(1.0).world, 0.3f, 1e-5f) && nearly(at(1.0).player, kUnscaled), "1.5 s, the world at 30%, Dylan at full speed");
        WritePrivateProfileStringW(L"Settings", L"enabled", L"false", menu.c_str());
        check(waitFor([&] { bw::tick(); return readLog(log).find("settings: Enabled=0: a perfect dodge's slow motion is the game's (4 from the MODS menu)") != npos; }, 4000), "enabled=false from the MODS menu: off");
        before = g_curveRequests.size();
        bw::dodge(); r = bw::play();
        {
            bool game = true;
            for (size_t i = 0; i < 34 && i < r.size(); i++) game = game && nearly(r[i].world, gameCurve((i + 1) * kUnscaled), 1e-3f) && r[i].player == kUnscaled * r[i].world && !r[i].jumpOwn;
            check(g_curveRequests.size() == before + 1 && game && r.size() >= 36 && r.size() <= 45 && r.back().world == 1, "switched off: one request, the game's 0.6 s of slow motion for everyone, nothing else");
        }
        DeleteFileW(menu.c_str());
        WritePrivateProfileStringW(L"WitchTime", nullptr, nullptr, ini.c_str());
        WritePrivateProfileStringW(L"WitchTime", L"Duration", L"0.2", ini.c_str());
        check(waitFor([&] { bw::tick(); return readLog(log).find("settings: witch time after a perfect dodge: 1 s with") != npos; }, 4000), "Duration=0.2 is taken as 1 s");
        WritePrivateProfileStringW(L"WitchTime", nullptr, nullptr, ini.c_str());
        check(waitFor([&] { bw::tick(); return readLog(log).rfind("settings: witch time after a perfect dodge: 4 s with") > readLog(log).rfind("settings: witch time after a perfect dodge: 1 s with"); }, 4000) &&
              readLog(log).rfind("settings: witch time after a perfect dodge: 1 s with") != npos, "(the defaults again)");

        // a second perfect dodge while the first one's witch time runs
        bw::dodge();
        for (int i = 0; i < 90; i++) t = bw::tick();
        check(nearly(t.world, 0.2f, 1e-5f) && nearly(t.player, kUnscaled) && t.jumpOwn, "1.5 s into a witch time: the world at 20%, Dylan free");
        bw::dodge();
        for (int i = 0; i < 12; i++) t = bw::tick();
        check(t.world < 0.1f && t.player == kUnscaled * t.world && !t.jumpOwn, "a second perfect dodge: its own slow motion comes first, Dylan slows with the world again");
        for (int i = 0; i < 48; i++) t = bw::tick();
        check(nearly(t.world, 0.2f, 1e-5f) && nearly(t.player, kUnscaled) && t.jumpOwn, "then free again, the world at 20%");
        r = bw::play();
        check(r.size() >= 178 && r.size() <= 190 && r.back().world == 1 && !r.back().jumpOwn && r.back().player == kUnscaled, "and the witch time lasts 4 s from the second dodge");

        // switched off while one runs; Enabled
        bw::dodge();
        for (int i = 0; i < 60; i++) t = bw::tick();
        WritePrivateProfileStringW(L"General", L"Enabled", L"0", ini.c_str());
        check(waitFor([&] { t = bw::tick(); return t.world == 1; }, 4000) && t.player == kUnscaled, "Enabled=0 while one runs: the world is back at full speed at once");
        for (int i = 0; i < 3; i++) t = bw::tick();
        check(!t.jumpOwn && !t.cameraOwn && t.jump == kUnscaled, "and the player's systems are back on the game's clock");
        r = bw::play();
        check(readLog(log).rfind("settings: Enabled=0: a perfect dodge's slow motion is the game's") > readLog(log).rfind("settings: witch time after a perfect dodge"), "log: Enabled=0");
        before = g_curveRequests.size(); bw::dodge(); r = bw::play();
        check(g_curveRequests.size() == before + 1 && r.size() <= 45, "a perfect dodge is the game's");
        WritePrivateProfileStringW(L"General", nullptr, nullptr, ini.c_str());
        check(waitFor([&] { bw::tick(); return readLog(log).rfind("settings: witch time after a perfect dodge") > readLog(log).rfind("settings: Enabled=0"); }, 4000), "(on again)");

        // the scheduler's array ends where readable memory ends: the walk has to stop there by itself
        {
            uint8_t* pages = (uint8_t*)VirtualAlloc(nullptr, 0x8000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            DWORD old; VirtualProtect(pages + 0x7000, 0x1000, PAGE_NOACCESS, &old);
            uint8_t* edge = pages + 0x7000 - bw::N * bullet::kSysSize;
            memcpy(edge, bw::g_systems, bw::N * bullet::kSysSize);
            bw::g_ctx = edge + bw::kHitStop * bullet::kSysSize;
            bw::hideList(true);                                                  // the plan from the first walk is stale: the walk runs again, over the copy at the edge
            bw::dodge(); r = bw::play();
            check(nearly(at(1.5).world, 0.2f, 1e-5f) && nearly(at(1.5).player, kUnscaled) && at(1.5).jumpOwn && at(1.5).dashOwn && !at(1.5).restOwn && !r.back().jumpOwn && !r.back().dashOwn && r.back().world == 1,
                  "a list of systems that ends at the end of readable memory: the walk stops there, the witch time works, everything comes back");
            check(readLog(log).find("caught in the witch time's clocks") == npos && readLog(log).find("caught in the timescale update") == npos, "and nothing faulted on the way");
            check(readLog(log).find("witch time: the player's systems, read again (the game's list had changed): 59 of the 59 the mod knows are in the game's list of 66 (read in ") != npos,
                  "the log says the list was read again");
            bw::hideList(false);
            bw::g_ctx = nullptr;
            VirtualFree(pages, 0, MEM_RELEASE);                                  // the plan now points into memory that is gone: stale again at the next witch time
            bw::dodge(); r = bw::play();
            check(nearly(at(1.5).world, 0.2f, 1e-5f) && nearly(at(1.5).player, kUnscaled) && at(1.5).jumpOwn && r.back().world == 1 && !r.back().jumpOwn,
                  "a plan that points into memory that is gone: read again from the game's list, the witch time works");
        }
        // a game whose list has only a few of the systems the mod knows: half a player on his own clock would be
        // worse than none, so that witch time is given up
        {
            bw::buildFew();
            bw::g_ctx = bw::g_few + bw::kHitStop * bullet::kSysSize;
            bw::hideList(true);
            bw::dodge(); r = bw::play();
            bool own = false, with = true;
            for (const Tick& x : r) { own = own || x.fewOwn || x.jumpOwn || x.dashOwn || x.cameraOwn; with = with && x.player == kUnscaled * x.world; }
            check(at(0.3).world < 0.2f && at(1.0).world == 1 && at(2.0).world == 1 && with && !own,
                  "a list with too few of the player's systems: the game's own slow motion plays, then the world is back at full speed; Dylan never leaves the world's clock, no system keeps another clock");
            check(readLog(log).find("witch time: only 2 of the 60 clocks of the player's systems could be taken over (2 of 59 systems found in the game's list of 4): the witch time ends here") != npos, "and the log says so");
            check(readLog(log).find("witch time: the list as the mod read it: 4 systems, 0 of them without a name the mod reads, from \"heron::encounter::update_encounter::system\" to \"heron::dash::update::system\"; it ended at an object that is not a system; 0 slots did not hold the game's clock, 0 systems share their list of environments with one the mod does not know, 0 of the plan were stale") != npos,
                  "with what the mod read of the list");
            before = g_curveRequests.size();
            bw::dodge(); r = bw::play();
            check(g_curveRequests.size() == before + 2 && at(1.0).world == 1, "(the next perfect dodge tries again, with the same end)");
            bw::hideList(false);
            bw::g_ctx = nullptr;
            bw::dodge(); r = bw::play();
            check(nearly(at(1.5).world, 0.2f, 1e-5f) && nearly(at(1.5).player, kUnscaled) && at(1.5).jumpOwn && r.back().world == 1 && !r.back().jumpOwn, "with the whole list again a perfect dodge has its witch time");
        }
        // the world's clocks are not in its table: the player stays with the world
        {
            uint32_t n = 0; memcpy(sw::g_world + bullet::kWorldEnvCount, &n, 4);
            bw::dodge(); r = bw::play();
            check(nearly(at(1.5).world, 0.2f, 1e-5f) && at(1.5).player == kUnscaled * at(1.5).world && !at(1.5).jumpOwn && r.back().world == 1,
                  "a world whose table of environments has no clocks: the world slows for 4 s, Dylan stays on the world's clock, no system is touched");
            check(readLog(log).find("witch time: the world's clocks are not where the mod expects them: the player stays on the world's clock") != npos, "and the log says so");
            n = 5; memcpy(sw::g_world + bullet::kWorldEnvCount, &n, 4);
        }
        // a fault in the mod's code must not reach the game: the list of running curves cannot be read when the mod looks
        {
            CurveItem* items = g_curveItems;
            for (int i = 0; i < 3; i++) {
                bw::dodge();
                g_breakCurveList = true; int tc2 = g_timescaleCalls;
                ((bw::TimescalesFn)code(g_timescalesFn))(g_curveDb, g_worldScale);
                g_breakCurveList = false; memcpy(g_curveDb + bullet::kDbActive, &items, 8);
                check(g_timescaleCalls == tc2 + 1, i ? "again" : "a list of running curves the mod cannot read: the fault is caught, the game's update has run");
            }
            check(readLog(log).find("no more witch time for this session") != npos, "after three faults the witch time stops for the session and the log says so");
            r = bw::play(1200);                                          // (what is still in the game's list runs out)
            before = g_curveRequests.size();
            bw::dodge(); r = bw::play();
            check(g_curveRequests.size() == before + 1 && r.size() <= 45 && !r[20].jumpOwn && r[20].player == kUnscaled * r[20].world, "from then on a perfect dodge is the game's");
            *factorX = 1.5f; bw::tick(); bw::tick(); t = bw::tick();
            check(t.player == kUnscaled && *writtenX == 0 && *hookedX == 0, "and another mod's factor is not applied any more either: WitchTime_Hooked is 0, the other mod writes the step itself again");
            *factorX = 1;
        }
        r = bw::play();
    }

    if (argc > 2) {
        std::wstring dir2 = argv[2]; if (dir2.back() != L'\\') dir2 += L'\\';
        std::wstring dll2 = dir2 + L"witchtime.dll";
        HMODULE second = LoadLibraryW(dll2.c_str());
        check(second != nullptr, "a second copy loads");
        check(waitFor([&] { return GetModuleHandleW(dll2.c_str()) == nullptr; }, 4000), "the second copy stands down and unloads");
        check(readLog(dir2 + L"witchtime.log").find("inactive: another copy of WitchTime is already in charge") != npos, "and its log says why");
    }
    FreeLibrary(m);
    Sleep(300);
    int mc = g_meleeCalls; sw::frame();
    check(GetModuleHandleW(dll.c_str()) != nullptr && g_meleeCalls == mc + 1, "FreeLibrary does not unload the active copy");
#endif
    printf(g_fail ? "%d of %d FAILED\n" : "all %d passed\n", g_fail ? g_fail : g_checks, g_checks);
    return g_fail ? 1 : 0;
}
