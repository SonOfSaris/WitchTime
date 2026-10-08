// WitchTime for CONTROL Resonant: bullet time after a perfect dodge.
//
// The game: a perfect dodge asks the timescale curve database for the curve "perfect_dodge" (0.6 s; the world
// at 10% -> 1% -> 19% -> 10% of its speed), and everything slows with it, Dylan too.
// The mod: that slow motion plays as the game made it. Then the world stays slow while Dylan's clock runs
// free, for as long as the setting says (bullet.h has the how and why). The hooks:
//   - the request for a timescale curve and the update of the running curves: the mod's own entry in the
//     game's list, and the world's speed;
//   - the engine's clock tick: the copy of the clock the player's systems read follows it at once;
//   - the system that writes every character's time step (heron::hit_stop::update_timescales, two forms)
//     and the engine's own writer (coregame::local_time_scale::update, two systems in two forms): the
//     player's own step (coregame::component::LocalTimeScale: animation, movement model, attacks) is
//     multiplied right after the game has written it, and before the hit stop system runs the player's
//     systems get the copy of the clock;
//   - the melee update of the player (heron::melee_attack::on_update): the one place that names the
//     player's entity every frame. The frame step it is handed gets the mod's factor, so the game's own
//     hit stop check in it (an attack in hit stop does not turn toward its target) fires for a real hit
//     stop only.
// Another mod that scales the player's time step as well (CombatRebalance's attack speed) hands its factor
// over through WitchTime_PlayerFactor, so one mod writes the step and nothing is multiplied twice.
// The game's code is found by byte patterns; on a build where something is missing the mod says so once
// (unless VersionWarning is off) and leaves. Nothing is written to a save and no game file is touched.
#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <share.h>
#include <string>
#include "MinHook.h"
#include "patterns.h"
#include "player.h"
#include "bullet.h"

#ifndef WT_VERSION
#define WT_VERSION "1.0"
#endif

// For other mods. A mod that would otherwise multiply the player's time step itself writes its factor into
// WitchTime_PlayerFactor (1 = none) while WitchTime_Hooked is 1; WitchTime multiplies the step by it together
// with its own. WitchTime_Written is the fixed step WitchTime wrote last (0 = none), so the other mod can tell
// that its factor is in the step.
extern "C" __declspec(dllexport) volatile float WitchTime_PlayerFactor = 1;
extern "C" __declspec(dllexport) volatile float WitchTime_Written = 0;
extern "C" __declspec(dllexport) volatile int WitchTime_Hooked = 0;
// Marks a copy that guards against a second instance.
extern "C" __declspec(dllexport) const int WitchTime_InstanceGuard = 1;
// Third-party notices, embedded in the binary (BSD-2-Clause requires them with binary distributions).
extern "C" __declspec(dllexport) const char WitchTime_ThirdPartyNotices[] =
#include "third_party_notices.inc"
    ;

namespace {

const char kVersion[] = WT_VERSION;

HMODULE g_self = nullptr;
std::wstring g_dir;      // folder of this DLL, with trailing backslash
FILE* g_log = nullptr;
CRITICAL_SECTION g_logLock;
DWORD g_t0 = 0;
int g_menuValues = 0;
bool g_hooked = false;
HANDLE g_instanceMutex = nullptr;

// Settings as the game thread reads them (written by the settings thread).
struct Config { bool enabled = true, versionWarning = true; float duration = 4, world = 0.2f, player = 1; };
Config g_cfg;                           // the settings thread's copy, for the log and the message
std::atomic<bool> g_on{false};
std::atomic<float> g_duration{4}, g_world{0.2f}, g_player{1};

// Game builds this version was checked on, by PE timestamp, with the version the game calls them. Any other
// build is tried all the same: the patterns decide whether the mod can run, not the timestamp.
struct KnownBuild { DWORD stamp; const char* version; };
const KnownBuild kKnownBuilds[] = { { 0x6ABA5BB8, "1.4.0" },      // the update of 1 October 2026
                                    { 0x6ABEF018, "1.4.1" } };    // Steam build 25673981
bool g_buildKnown = false;
std::string g_gameVersion, g_fileVersion, g_buildDate;

// The player, as the melee update sees it, and the world, as the hooks last saw it.
std::atomic<uint64_t> g_entity{~0ull};
std::atomic<uint64_t> g_entityStamp{0};
std::atomic<const uint8_t*> g_worldPtr{nullptr};
std::atomic<uint64_t> g_worldStamp{0};
std::atomic<float> g_fixed{-1}, g_variable{-1}, g_factor{1};    // player::Applied
std::atomic<unsigned> g_applied{0}, g_wanted{0};
std::atomic<int> g_faults{0};
std::atomic<uint64_t> g_said{0};

// ---- the hooks
using MeleeFn = void (*)(uint64_t* row, void* world, void* a3, void* a4, float* frameStep, void* a6);
using StepOneFn = void (*)(uint8_t** world, uint64_t archetype, void* ctx);
using StepAllFn = void (*)(uint8_t** world, void* a2, void* ctx);
MeleeFn g_origMelee = nullptr;
StepOneFn g_origStepOne = nullptr;
StepAllFn g_origStepAll = nullptr;
StepOneFn g_origLocalOne[2] = {};
StepAllFn g_origLocalAll[2] = {};
using StartCurveFn = int (*)(uint8_t* db, uint32_t curve, float duration);
using TimescalesFn = void (*)(uint8_t* db, uint8_t* out);
using ClockTickFn = void (*)(uint8_t* fixed);
StartCurveFn g_origStartCurve = nullptr;
TimescalesFn g_origTimescales = nullptr;
ClockTickFn g_origClockTick = nullptr;
bool g_bulletHooked = false;
bullet::Runs g_btRuns;                  // under g_btLock
std::atomic_flag g_btLock = ATOMIC_FLAG_INIT;
std::atomic<int> g_btRunCount{0};
std::atomic<float> g_btUnslow{0}, g_btAim{1};       // what the newest witch time says: written by the curves' update, read by the time step hooks
bullet::Clocks g_btClocks;              // what the player's systems read while the player's clock is free; under g_btClockLock
bullet::Slots g_btSlots;                // where the mod's copies are right now; under g_btClockLock
bullet::Plan g_btPlan;                  // where the player's systems are, from the one walk over the game's list; under g_btClockLock
std::atomic<bool> g_btSurveyed{false};  // that walk has been made
std::atomic<const uint8_t*> g_btFixed{nullptr};        // the world's FixedTime the copy is made of
bool g_btEngaged = false;               // the player's systems were handed the copies (or the mod was at it); under g_btClockLock
std::atomic_flag g_btClockLock = ATOMIC_FLAG_INIT;
std::atomic<bool> g_btSwapped{false};   // g_btEngaged, for a look without the lock
std::atomic<float> g_btFactor{1};       // world's step -> player's step, as the last look at the clocks had it
std::atomic<unsigned> g_btLines{0};
std::atomic<uint64_t> g_btStarted{0};   // GetTickCount64 at the perfect dodge that started the running witch time
std::atomic<int> g_btUpdates{0};        // the game's updates of its curves since then
std::atomic<bool> g_btAbort{false};     // the running witch time is given up (the player's systems were not found): the world goes back to its speed

void logf(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_logLock);
    fprintf(g_log, "[%7.3f] ", (GetTickCount() - g_t0) / 1000.0);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}
bool sayOnce(unsigned bit) { return !(g_said.fetch_or(1ull << bit) & (1ull << bit)); }
// (log bits 1: the first perfect dodge that starts a witch time, 2: it is in the game's list, 3: the
// player's clock comes free, 4: the world's clocks are not where expected, 5: too few of the player's
// systems found, 6: the player's time step was never found)
void fault(const char* where) {
    int f = g_faults.fetch_add(1) + 1;
    logf("a fault in the mod's own code was caught in %s (%d of 3)%s", where, f, f >= 3 ? ": no more witch time for this session" : "");
    if (f >= 3) WitchTime_Hooked = 0;                   // another mod that handed its factor over writes the step itself again
}
std::wstring wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// ---------------------------------------------------------------- settings
double iniNum(const wchar_t* sec, const wchar_t* key, double def, const std::wstring& file) {
    wchar_t buf[64]; GetPrivateProfileStringW(sec, key, L"", buf, 64, file.c_str());
    if (!buf[0]) return def;
    wchar_t* e = nullptr; double v = wcstod(buf, &e);
    return e != buf ? v : def;
}

// Mod Settings Menu saves what the player changes on Options > MODS under [Settings] in
// ModMenuConfig\<id>.ini, in the folder of the descriptor (witchtime.menu.json, next to this DLL).
bool menuNum(const wchar_t* key, double& v, const std::wstring& file) {
    wchar_t buf[64]; GetPrivateProfileStringW(L"Settings", key, L"", buf, 64, file.c_str());
    if (!buf[0]) return false;
    if (!_wcsicmp(buf, L"true")) { v = 1; return true; }
    if (!_wcsicmp(buf, L"false")) { v = 0; return true; }
    wchar_t* e = nullptr; v = wcstod(buf, &e);
    return e != buf;
}

std::wstring iniPath()  { return g_dir + L"witchtime_config.ini"; }
std::wstring menuPath() { return g_dir + L"ModMenuConfig\\witchtime.ini"; }

float clampf(double v, double lo, double hi) { return (float)(v < lo ? lo : v > hi ? hi : v); }

void loadSettings() {
    Config c;                                 // from the defaults every time: a removed line goes back to its default
    std::wstring f = iniPath();
    c.enabled = iniNum(L"General", L"Enabled", 1, f) != 0;
    c.versionWarning = iniNum(L"General", L"VersionWarning", 1, f) != 0;
    double duration = iniNum(L"WitchTime", L"Duration", 4, f);
    double world = iniNum(L"WitchTime", L"WorldSpeed", 20, f), player = iniNum(L"WitchTime", L"PlayerSpeed", 100, f);
    // A value saved by the menu wins over the ini.
    int n = 0; double v;
    std::wstring m = menuPath();
    if (GetFileAttributesW(m.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (menuNum(L"enabled", v, m))         { c.enabled = v != 0; n++; }
        if (menuNum(L"version_warning", v, m)) { c.versionWarning = v != 0; n++; }
        if (menuNum(L"duration_ms", v, m))     { duration = v / 1000; n++; }         // the menu keeps whole numbers
        if (menuNum(L"world_speed", v, m))     { world = v; n++; }
        if (menuNum(L"player_speed", v, m))    { player = v; n++; }
    }
    c.duration = clampf(duration, bullet::kMinDuration, 20);
    c.world = clampf(world, 2, 100) / 100.0f; c.player = clampf(player, 25, 300) / 100.0f;
    g_menuValues = n;
    g_cfg = c;
    g_duration = c.duration; g_world = c.world; g_player = c.player; g_on = c.enabled;
    g_btLines = 0;
}

void logSettings() {
    if (g_cfg.enabled) logf("settings: witch time after a perfect dodge: %g s with the game's own slow motion, then the world at %g%% and Dylan at %g%% of their speed (%d from the MODS menu)",
                            g_cfg.duration, g_cfg.world * 100, g_cfg.player * 100, g_menuValues);
    else logf("settings: Enabled=0: a perfect dodge's slow motion is the game's (%d from the MODS menu)", g_menuValues);
}

void fileTime(const std::wstring& f, FILETIME& t) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    t = GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &fad) ? fad.ftLastWriteTime : FILETIME{};
}

// ---------------------------------------------------------------- the player's time step
// What another mod asks for on top (WitchTime_PlayerFactor), kept within reason.
float extraFactor() {
    const float x = WitchTime_PlayerFactor;
    return x > 0.01f && x < 100 ? x : 1;
}

// The melee update of one entity (only the player has a combo graph): names the player's entity, and says
// what factor of the mod's is in the player's step right now. No C++ objects in here.
struct MeleeOut { uint64_t entity; float baked; };
int guardedMelee(uint64_t* row, MeleeOut* o) {
    __try {
        if (!row[player::kRowChunk]) return 0;
        const size_t i = (size_t)row[player::kRowIndex];
        o->entity = player::rd<uint64_t>((const uint8_t*)row[player::kRowChunk], player::kChunkEntities + i * 8);
        const float* step = row[player::kRowTimeStep] ? (const float*)((const uint8_t*)row[player::kRowTimeStep] + i * player::kTimeStepSize) : nullptr;
        player::Applied a{g_fixed.load(std::memory_order_relaxed), g_variable.load(std::memory_order_relaxed), g_factor.load(std::memory_order_relaxed)};
        o->baked = player::baked(step, a);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

void hkMelee(uint64_t* row, void* world, void* a3, void* a4, float* frameStep, void* a6) {
    float mine; float* pass = frameStep;
    if (row && frameStep && g_faults.load(std::memory_order_relaxed) < 3) {
        MeleeOut o;
        const int r = guardedMelee(row, &o);
        if (r < 0) fault("the melee update");
        else if (r > 0) {
            const uint64_t now = GetTickCount64();
            g_entity.store(o.entity, std::memory_order_relaxed); g_entityStamp.store(now, std::memory_order_relaxed);
            if (world && *(const uint8_t**)world) { g_worldPtr.store(*(const uint8_t**)world, std::memory_order_relaxed); g_worldStamp.store(now, std::memory_order_relaxed); }
            // The game compares the frame's step with the character's own to see a hit stop (an attack in hit
            // stop does not turn toward its target). With the mod's factor in the player's step the frame's
            // step gets the same factor, so only a real hit stop differs.
            if (o.baked != 1) { mine = *frameStep * o.baked; pass = &mine; }
            // The player's clock is wanted free, but the time step hooks never found the player: say so once.
            if (g_btFactor.load(std::memory_order_relaxed) != 1 && g_applied.load(std::memory_order_relaxed) == 0 && g_wanted.fetch_add(1) > 600 && sayOnce(6))
                logf("the player's time step was not found: Dylan slows with the world (game build changed?)");
        }
    }
    g_origMelee(row, world, a3, a4, pass, a6);
}

int guardedStep(uint8_t** world, int archetype, float factor, bool fresh) {
    __try {
        if (!world || !*world) return 0;
        float* step = player::timeStepOf(*world, g_entity.load(std::memory_order_relaxed), archetype);
        if (!step) return 0;
        player::Applied a{g_fixed.load(std::memory_order_relaxed), g_variable.load(std::memory_order_relaxed), g_factor.load(std::memory_order_relaxed)};
        // The hit stop system has just written this step: what it holds is the game's, also when it is the very
        // number the mod wrote last (a witch time's step, multiplied back up, is the plain step again).
        if (fresh) { a.fixed = -1; a.variable = -1; }
        if (!player::apply(step, factor, a)) return 0;
        g_fixed = a.fixed; g_variable = a.variable; g_factor = a.factor;
        WitchTime_Written = a.fixed;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// After the game has written the time steps (of one archetype, or of all: archetype -1). The player's gets
// the factor that sets the player's clock free of the world's, and whatever another mod asks for on top.
// written = the system that has just run writes every character's step (the hit stop system does; the
// engine's own two only those of characters with a ratio).
void afterStep(uint8_t** world, int archetype, bool written) {
    if (world && *world) { g_worldPtr.store(*world, std::memory_order_relaxed); g_worldStamp.store(GetTickCount64(), std::memory_order_relaxed); }
    if (g_faults.load(std::memory_order_relaxed) >= 3) return;
    const bool fresh = GetTickCount64() - g_entityStamp.load(std::memory_order_relaxed) <= 250;        // else no melee update lately: the player is not known
    const float f = fresh ? g_btFactor.load(std::memory_order_relaxed) * extraFactor() : 1;
    if (f == 1) {
        if (written) { g_fixed = -1; g_variable = -1; g_factor = 1; WitchTime_Written = 0; }        // nothing of the mod's is in the step
        return;
    }
    const int r = guardedStep(world, archetype, f, written);
    if (r < 0) fault("the time step hook");
    else if (r > 0 && g_applied.fetch_add(1) == 0) logf("the player's time step is found (x%.2f applied)", f);
}
void bulletClocks(uint8_t** world, void* system);
void hkStepOne(uint8_t** world, uint64_t archetype, void* ctx) { bulletClocks(world, ctx); g_origStepOne(world, archetype, ctx); afterStep(world, (uint16_t)archetype, true); }
void hkStepAll(uint8_t** world, void* a2, void* ctx) { bulletClocks(world, ctx); g_origStepAll(world, a2, ctx); afterStep(world, -1, true); }
// The engine's own time step update (two systems, each in two forms): the same afterwards.
void hkLocalOne0(uint8_t** world, uint64_t archetype, void* ctx) { g_origLocalOne[0](world, archetype, ctx); afterStep(world, (uint16_t)archetype, false); }
void hkLocalOne1(uint8_t** world, uint64_t archetype, void* ctx) { g_origLocalOne[1](world, archetype, ctx); afterStep(world, (uint16_t)archetype, false); }
void hkLocalAll0(uint8_t** world, void* a2, void* ctx) { g_origLocalAll[0](world, a2, ctx); afterStep(world, -1, false); }
void hkLocalAll1(uint8_t** world, void* a2, void* ctx) { g_origLocalAll[1](world, a2, ctx); afterStep(world, -1, false); }

// ---------------------------------------------------------------- the witch time
bullet::Config bulletConfig() {
    bullet::Config c;
    c.on = g_on.load(std::memory_order_relaxed) && !g_btAbort.load(std::memory_order_relaxed) && g_faults.load(std::memory_order_relaxed) < 3;
    c.duration = g_duration.load(std::memory_order_relaxed);
    c.world = g_world.load(std::memory_order_relaxed); c.player = g_player.load(std::memory_order_relaxed);
    return c;
}

// The perfect dodge asks for its slow motion: that request goes through as it is. Then one more for the same
// curve, as long as the witch time: the game keeps it in its list for that long and counts its seconds.
int hkStartCurve(uint8_t* db, uint32_t curve, float duration) {
    const int id = g_origStartCurve(db, curve, duration);
    const bullet::Config c = bulletConfig();
    if (!c.on || !db) return id;
    const int mine = g_origStartCurve(db, curve, c.duration);
    while (g_btLock.test_and_set(std::memory_order_acquire)) YieldProcessor();
    g_btRuns.add(mine, id); g_btRunCount.store(g_btRuns.n, std::memory_order_relaxed);
    g_btLock.clear(std::memory_order_release);
    g_btStarted.store(GetTickCount64(), std::memory_order_relaxed); g_btUpdates.store(0, std::memory_order_relaxed);
    if (sayOnce(1)) logf("witch time: a perfect dodge starts one (timescale curve 0x%08x, requests %d and %d): %g s in all", curve, id, mine, c.duration);
    return id;
}

int guardedBulletAfter(const bullet::Config& c, uint8_t* db, uint8_t* out, bullet::Now* now) {
    while (g_btLock.test_and_set(std::memory_order_acquire)) YieldProcessor();
    int r = 1;
    __try { *now = bullet::after(c, db, out, g_btRuns); }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = -1; g_btRuns.n = 0; }
    g_btRunCount.store(g_btRuns.n, std::memory_order_relaxed);
    g_btLock.clear(std::memory_order_release);
    return r;
}
// After the game has given its running slow motions their values: the mod's entries get the mod's.
void hkTimescales(uint8_t* db, uint8_t* out) {
    g_origTimescales(db, out);
    if (!g_btRunCount.load(std::memory_order_relaxed)) { g_btUnslow.store(0, std::memory_order_relaxed); g_btAbort.store(false, std::memory_order_relaxed); return; }
    const bullet::Config c = bulletConfig();
    bullet::Now now;
    g_btUpdates.fetch_add(1, std::memory_order_relaxed);
    if (guardedBulletAfter(c, db, out, &now) < 0) { g_btUnslow.store(0, std::memory_order_relaxed); fault("the timescale update"); return; }
    g_btAim.store(bullet::target(c, now.shape), std::memory_order_relaxed);
    g_btUnslow.store(now.active ? now.shape.unslow : 0, std::memory_order_relaxed);
    if (now.started && sayOnce(2)) logf("witch time: it is in the game's list of running slow motions (%g s; the game's own slow motion takes the first %g s)", now.total, now.game);
    if (now.active && now.shape.unslow > 0 && sayOnce(3))
        logf("witch time: the game's own slow motion is over (%.2f s); the world goes on at %g%%, the player's clock comes free", now.t, c.world * 100);
    static std::atomic<int> overLines{0};
    if (now.ended && overLines.fetch_add(1) < 4)
        logf("witch time: over, the world is back at its own speed (%.2f s by the clock, %d updates of the game's slow motions)",
             (GetTickCount64() - g_btStarted.load(std::memory_order_relaxed)) / 1000.0, g_btUpdates.load(std::memory_order_relaxed));
}

// May this memory be read? The walk over the scheduler's systems asks before every step it takes. VirtualQuery
// is slow in the game (tens of microseconds), so the readable stretches found are kept for the rest of the
// walk: the objects are in one, the names in a few. Under g_btClockLock; forgetReadable() before each walk.
struct ReadableStretch { uintptr_t lo, hi; };
ReadableStretch g_readable[64]; int g_readableCount = 0, g_readableNext = 0;
void forgetReadable() { g_readableCount = 0; g_readableNext = 0; }
bool readableMemory(const void* p, size_t n) {
    const uintptr_t a = (uintptr_t)p, b = a + n;
    if (b < a) return false;
    for (int i = 0; i < g_readableCount; i++) if (a >= g_readable[i].lo && b <= g_readable[i].hi) return true;
    uintptr_t first = 0, end = 0;
    for (uintptr_t at = a; at < b; at = end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)at, &mbi, sizeof mbi)) return false;
        const DWORD kind = mbi.Protect & 0xff;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || kind == PAGE_NOACCESS || kind == PAGE_EXECUTE || !kind) return false;
        if (!first) first = (uintptr_t)mbi.BaseAddress;
        end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (end <= at) return false;
    }
    if (g_readableCount < 64) g_readable[g_readableCount++] = ReadableStretch{first, end};
    else { g_readable[g_readableNext] = ReadableStretch{first, end}; g_readableNext = (g_readableNext + 1) % 64; }
    return true;
}

// The walk over the game's list of systems; under g_btClockLock. Returns the milliseconds it took, -1 on a fault.
double guardedSurvey(void* system) {
    __try {
        LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
        forgetReadable();
        bullet::survey((uint8_t*)system, g_btPlan, readableMemory);
        QueryPerformanceCounter(&t1);
        return (t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
void logSurvey(const bullet::Plan& p, double ms, const char* when) {
    char odd[224] = "";
    if (p.unnamed) sprintf_s(odd, " (%d without a name the mod reads)", p.unnamed);
    if (p.shared) sprintf_s(odd + strlen(odd), sizeof odd - strlen(odd), " (%d left alone: a system the mod does not know reads the same list of environments)", p.shared);
    logf("witch time: the player's systems%s: %d of the %d the mod knows are in the game's list of %d (read in %.0f ms)%s", when, p.named, bullet::kSystemCount, p.systems, ms, odd);
}

// The clocks of the player's systems; under g_btClockLock. 1 = looked at the clocks, 2 = the systems were also
// handed the mod's copies (w says how many), 3 = they got the game's clocks back (restored), 4 = too few of
// them were found and what was handed over is taken back, 0 = the world's clocks are not where the mod
// expects them. No C++ objects in here.
int guardedClocks(uint8_t** world, void* system, float unslow, float aim, bool want, bullet::Swap* w, int* restored, float* factor, double* surveyMs) {
    __try {
        const uint8_t* fixed = nullptr; const uint8_t* variable = nullptr;
        if (want && world && *world) { fixed = bullet::envOf(*world, bullet::kFixedTime); variable = bullet::envOf(*world, bullet::kVariableTime); }
        if (!want || !fixed) {                              // the way back goes by the mod's own list: nothing of the world is needed
            if (g_btEngaged) { forgetReadable(); *restored = bullet::release(g_btSlots, g_btClocks, readableMemory); g_btEngaged = false; return want ? 0 : 3; }
            return want ? 0 : 1;
        }
        bullet::refresh(g_btClocks, fixed, variable, unslow, aim);
        *factor = bullet::factor(fixed, unslow, aim);
        g_btFixed.store(fixed, std::memory_order_relaxed);
        if (g_btEngaged) return 1;
        g_btEngaged = true;                                 // before the swap: whatever happens in it, the way back is taken later
        forgetReadable();
        *w = bullet::engage(g_btPlan, fixed, variable, g_btClocks, g_btSlots, readableMemory);
        if (w->swapped + w->already >= bullet::kMinClocks) return 2;
        *surveyMs = guardedSurvey(system);                  // the plan is stale (or was never made): the game's list is read again
        if (*surveyMs < 0) return -1;
        forgetReadable();
        *w = bullet::engage(g_btPlan, fixed, variable, g_btClocks, g_btSlots, readableMemory);
        if (w->swapped + w->already >= bullet::kMinClocks) return 2;
        *restored = bullet::release(g_btSlots, g_btClocks, readableMemory); g_btEngaged = false;
        return 4;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
// The engine has moved a clock on by one tick. While the player's systems read the mod's copy of the world's
// clock, the copy follows at once: its time and tick count are never behind the game's.
int guardedTickCopy(const uint8_t* fixed, float unslow, float aim) {
    __try { if (g_btEngaged) bullet::copyClock(g_btClocks.fixed, fixed, bullet::kFixedSize, bullet::factor(fixed, unslow, aim)); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
void hkClockTick(uint8_t* fixed) {
    g_origClockTick(fixed);
    if (!g_btSwapped.load(std::memory_order_relaxed) || fixed != g_btFixed.load(std::memory_order_relaxed)) return;
    const float unslow = g_faults.load(std::memory_order_relaxed) >= 3 ? 0 : g_btUnslow.load(std::memory_order_relaxed);
    while (g_btClockLock.test_and_set(std::memory_order_acquire)) YieldProcessor();
    const int r = guardedTickCopy(fixed, unslow, g_btAim.load(std::memory_order_relaxed));
    g_btClockLock.clear(std::memory_order_release);
    if (r < 0) fault("the clock tick");
}

// Before the game writes the characters' time steps (the hit stop system: its context is one of the
// scheduler's system objects). Nothing to do unless a witch time has the player's clock free, or just had.
void bulletClocks(uint8_t** world, void* system) {
    if (!g_bulletHooked) return;
    if (!g_btSurveyed.load(std::memory_order_relaxed)) {    // the walk over the game's list, long before any witch time
        // Every 300th call until it has found the player's systems (20 tries at most): the first calls may
        // come with a system object that is not in the game's main list.
        static std::atomic<int> calls{0};
        const int call = calls.fetch_add(1, std::memory_order_relaxed);
        if (call % 300 == 0) {
            while (g_btClockLock.test_and_set(std::memory_order_acquire)) YieldProcessor();
            const double ms = guardedSurvey(system);
            const bullet::Plan p = g_btPlan;
            const bool enough = ms >= 0 && p.n * 2 >= bullet::kSystemCount;
            if (enough || call >= 300 * 19) g_btSurveyed.store(true, std::memory_order_relaxed);
            g_btClockLock.clear(std::memory_order_release);
            if (ms < 0) fault("the walk over the game's systems");
            else if (enough || call == 0) logSurvey(p, ms, "");
        }
    }
    float unslow = g_btUnslow.load(std::memory_order_relaxed);
    if (g_faults.load(std::memory_order_relaxed) >= 3 || g_btAbort.load(std::memory_order_relaxed)) unslow = 0;
    if (!(unslow > 0) && !g_btSwapped.load(std::memory_order_relaxed)) { g_btFactor.store(1, std::memory_order_relaxed); return; }
    const bool want = unslow > 0;
    bullet::Swap w; float factor = 1; int restored = 0; double surveyMs = -2;
    while (g_btClockLock.test_and_set(std::memory_order_acquire)) YieldProcessor();
    const int r = guardedClocks(world, system, unslow, g_btAim.load(std::memory_order_relaxed), want, &w, &restored, &factor, &surveyMs);
    if (r < 0 && !want) { g_btSlots.n = 0; g_btEngaged = false; }       // the way back itself failed: not tried again
    g_btSwapped.store(g_btEngaged, std::memory_order_relaxed);
    const bullet::Plan again = surveyMs >= 0 ? g_btPlan : bullet::Plan();
    g_btClockLock.clear(std::memory_order_release);
    if (surveyMs >= 0 && g_btLines.load(std::memory_order_relaxed) < 8) logSurvey(again, surveyMs, ", read again (the game's list had changed)");
    g_btFactor.store(r > 0 && r != 4 && want ? factor : 1, std::memory_order_relaxed);
    if (r < 0) { fault("the witch time's clocks"); return; }
    if (r == 4) {
        g_btAbort.store(true, std::memory_order_relaxed);
        if (sayOnce(5)) {
            logf("witch time: only %d of the %d clocks of the player's systems could be taken over (%d of %d systems found in the game's list of %d): the witch time ends here, "
                 "a perfect dodge keeps the game's own slow motion (game build changed?)", w.swapped + w.already, bullet::kClockCount, w.named, bullet::kSystemCount, w.systems);
            logf("witch time: the list as the mod read it: %d systems, %d of them without a name the mod reads, from \"%s\" to \"%s\"; it ended at %s; "
                 "%d slots did not hold the game's clock, %d systems share their list of environments with one the mod does not know, %d of the plan were stale", w.systems, w.unnamed, w.first, w.last,
                 w.end == 1 ? "an object that is not a system" : w.end == 2 ? "memory that cannot be read" : "the most systems the mod walks", w.other, w.shared, w.stale);
        }
        return;
    }
    if (r == 0) {
        if (sayOnce(4)) logf("witch time: the world's clocks are not where the mod expects them: the player stays on the world's clock (game build changed?)");
        return;
    }
    if ((r == 2 || r == 3) && g_btLines.fetch_add(1) < 8) {
        char odd[224] = "";
        if (w.other) sprintf_s(odd, " (%d left alone: not the game's clock where the mod expects it)", w.other);
        if (r == 2) logf("witch time: %d clocks are now the player's%s", w.swapped, odd);
        else logf("witch time: the player's systems are back on the game's clock (%d clocks)", restored);
    }
}

// ---------------------------------------------------------------- the game build, and the one message
// File version of an image from its version resource ("0.564.478.0"); empty when it has none. The game's
// files carry no other version: the number the game shows is only known for the builds listed above.
std::string fileVersionOf(HMODULE img) {
    HRSRC res = FindResourceW(img, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16));      // RT_VERSION
    HGLOBAL h = res ? LoadResource(img, res) : nullptr;
    const uint8_t* p = h ? (const uint8_t*)LockResource(h) : nullptr;
    DWORD n = p ? SizeofResource(img, res) : 0;
    for (DWORD i = 0; i + 16 <= n; i += 4) {
        uint32_t sig; memcpy(&sig, p + i, 4);
        if (sig != 0xFEEF04BD) continue;                    // start of VS_FIXEDFILEINFO; the file version follows at +8
        uint32_t ms, ls; memcpy(&ms, p + i + 8, 4); memcpy(&ls, p + i + 12, 4);
        char out[64]; sprintf_s(out, "%u.%u.%u.%u", ms >> 16, ms & 0xffff, ls >> 16, ls & 0xffff);
        return out;
    }
    return "";
}
// The day an image was built, from its PE timestamp: "1 October 2026".
std::string dateOf(DWORD stamp) {
    static const char* months[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
    time_t t = stamp; struct tm d{};
    if (gmtime_s(&d, &t)) return "";
    char out[40]; sprintf_s(out, "%d %s %d", d.tm_mday, months[d.tm_mon], 1900 + d.tm_year);
    return out;
}
bool dialogs() { return GetEnvironmentVariableW(L"WITCHTIME_NO_DIALOGS", nullptr, 0) == 0; }   // tests switch the pop-up off

// The one message box of this mod, when the game's code is not where the mod expects it: what happened,
// the versions, the problem, the likely cause and what a report should contain. The text goes into the log
// as well, shown or not.
void showMessage(const std::wstring& problem) {
    std::wstring game = g_buildKnown ? wide(g_gameVersion) : L"unknown to this mod";
    if (!g_fileVersion.empty()) game += L" (file version " + wide(g_fileVersion) + L")";
    std::wstring cause = g_buildKnown
        ? L"WitchTime supports this game version, so a game update is not the cause. Most likely another mod or tool has changed the game code WitchTime needs. Try the game without your other mods to find out which one."
        : L"WitchTime " + wide(kVersion) + L" does not know this game version. A game update has probably changed the code the mod needs: look for a newer version of WitchTime first.";
    std::wstring msg = L"WitchTime is switched off for this session. The game itself is not affected.\n\n"
        L"Mod version: " + wide(kVersion) + L"\n"
        L"Game version: " + game + L"\n"
        L"Game build date: " + wide(g_buildDate) + L"\n"
        L"Problem: " + problem + L"\n\n" + cause + L"\n\n"
        L"If you report this, please include:\n"
        L"- this message (Ctrl+C copies it)\n"
        L"- where you got the game: Steam, Epic or other\n"
        L"- the other mods you use\n"
        L"- the text of witchtime.log, in crmods\\WitchTime\n\n"
        L"To stop this message, set VersionWarning=0 in witchtime_config.ini, or switch it off under Options > MODS.";
    const bool show = g_cfg.versionWarning && dialogs();
    logf("message%s:", show ? "" : g_cfg.versionWarning ? " (not shown: dialogs are off)" : " (not shown: VersionWarning=0)");
    for (size_t a = 0; a < msg.size(); ) {
        size_t b = msg.find(L'\n', a);
        if (b == std::wstring::npos) b = msg.size();
        logf("  | %ls", msg.substr(a, b - a).c_str());
        a = b + 1;
    }
    if (show) MessageBoxW(nullptr, msg.c_str(), (L"WitchTime " + wide(kVersion)).c_str(), MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
}

// ---------------------------------------------------------------- install
bool hookedByOthers(const uint8_t* p) { return p[0] == 0xE9 || (p[0] == 0xFF && p[1] == 0x25); }

// Two mods that hook the same game function at the same time can lose one of the hooks: each reads the
// function's first bytes when it creates its hook and writes its jump when it enables it, and the second
// jump written wins when the two overlap. Mods that know this convention take turns: one holds this mutex
// from the first hook it creates to the last one it enables. (WitchTime does; CombatRebalance does.)
struct HookTurn {
    HANDLE h = nullptr;
    HookTurn() {
        wchar_t name[64]; swprintf_s(name, L"CONTROLResonant_HookInstall_%lu", GetCurrentProcessId());
        h = CreateMutexW(nullptr, FALSE, name);
        if (h) WaitForSingleObject(h, 15000);
    }
    ~HookTurn() { if (h) { ReleaseMutex(h); CloseHandle(h); } }
    HookTurn(const HookTurn&) = delete;
    HookTurn& operator=(const HookTurn&) = delete;
};

// Finds the game's code and hooks it. All or nothing: every pattern must match exactly once. problem = what
// was not found, for the message.
bool installHook(std::wstring& problem) {
    HMODULE exe = GetModuleHandleW(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)exe;
    auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)exe + dos->e_lfanew);
    const DWORD stamp = nt->FileHeader.TimeDateStamp;
    for (const KnownBuild& b : kKnownBuilds) if (b.stamp == stamp) { g_buildKnown = true; g_gameVersion = b.version; }
    g_fileVersion = fileVersionOf(exe); g_buildDate = dateOf(stamp);
    logf("exe %p timestamp 0x%08X (%s), file version %s: %s%s", exe, stamp, g_buildDate.c_str(), g_fileVersion.empty() ? "none" : g_fileVersion.c_str(),
         g_buildKnown ? "game version " : "not a build this version knows, trying anyway", g_buildKnown ? g_gameVersion.c_str() : "");
    const uint8_t* text = nullptr; size_t textSize = 0;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (memcmp(sec[i].Name, ".text", 5) == 0) { text = (uint8_t*)exe + sec[i].VirtualAddress; textSize = sec[i].Misc.VirtualSize; break; }
    if (!text) { logf("FAIL: no .text section"); problem = L"the game's code section was not found"; return false; }

    pat::StepCode c; const char* why = "";
    if (!pat::findSteps(text, textSize, c, why)) {
        logf("not found as expected: %s (game build changed?)", why);
        problem = L"game function \"" + wide(why) + L"\" not found"; return false;
    }
    pat::BulletCode b;
    pat::findBullet(text, textSize, b);
    const char* miss = !b.startCurve ? "the request for a timescale curve" : !b.timescales ? "the update of the timescale curves"
                     : !b.layout ? "the layout of the scheduler's systems, the world's environments and the engine's clock" : nullptr;
    if (miss) {
        logf("not found as expected: %s (game build changed?)", miss);
        problem = L"game code \"" + wide(miss) + L"\" not found"; return false;
    }

    // From here on this copy stays: the game's code is about to point into it.
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&hkMelee, &pinned);
    HookTurn turn;                                      // (released when this function returns)
    if (MH_Initialize() != MH_OK) { logf("FAIL: MinHook init"); problem = L"the hook library could not start"; return false; }
    struct { const uint8_t* at; void* hook; void** orig; } h[9] = {
        {c.melee, (void*)hkMelee, (void**)&g_origMelee}, {c.stepOne, (void*)hkStepOne, (void**)&g_origStepOne}, {c.stepAll, (void*)hkStepAll, (void**)&g_origStepAll},
        {c.localOne[0], (void*)hkLocalOne0, (void**)&g_origLocalOne[0]}, {c.localOne[1], (void*)hkLocalOne1, (void**)&g_origLocalOne[1]},
        {c.localAll[0], (void*)hkLocalAll0, (void**)&g_origLocalAll[0]}, {c.localAll[1], (void*)hkLocalAll1, (void**)&g_origLocalAll[1]},
        {b.timescales, (void*)hkTimescales, (void**)&g_origTimescales}, {b.startCurve, (void*)hkStartCurve, (void**)&g_origStartCurve},
    };
    bool others = false;
    for (auto& x : h) others |= hookedByOthers(x.at);
    logf("melee update at rva 0x%llx; time step updates at rva 0x%llx, 0x%llx (hit stop) and 0x%llx, 0x%llx, 0x%llx, 0x%llx (engine)", (unsigned long long)(c.melee - (const uint8_t*)exe),
         (unsigned long long)(c.stepOne - (const uint8_t*)exe), (unsigned long long)(c.stepAll - (const uint8_t*)exe), (unsigned long long)(c.localOne[0] - (const uint8_t*)exe),
         (unsigned long long)(c.localOne[1] - (const uint8_t*)exe), (unsigned long long)(c.localAll[0] - (const uint8_t*)exe), (unsigned long long)(c.localAll[1] - (const uint8_t*)exe));
    logf("timescale curves: the request at rva 0x%llx, the update at rva 0x%llx%s", (unsigned long long)(b.startCurve - (const uint8_t*)exe), (unsigned long long)(b.timescales - (const uint8_t*)exe),
         others ? "; another mod's hook is on one of these functions, this one goes on top" : "");
    bool ok = true;
    for (auto& x : h) ok = ok && MH_CreateHook((void*)x.at, x.hook, x.orig) == MH_OK;
    g_bulletHooked = true;                              // before the first call can come in
    // The time step hooks first (they do nothing until the melee update has named the player), the curves'
    // update before the request (nothing is asked for before the update can answer), the melee update last.
    const int order[9] = {6, 5, 4, 3, 2, 1, 7, 8, 0};
    for (int i : order) if (ok) ok = MH_EnableHook((void*)h[i].at) == MH_OK;
    if (!ok) {
        g_bulletHooked = false;
        for (auto& x : h) { MH_DisableHook((void*)x.at); MH_RemoveHook((void*)x.at); }
        logf("FAIL: hook"); problem = L"the hooks could not be set"; return false;
    }
    // The engine's clock tick: with it the copy of the clock is exact on every tick; without it the copy is
    // made once per update, which the witch time also works with.
    if (b.clockTick && MH_CreateHook((void*)b.clockTick, (void*)hkClockTick, (void**)&g_origClockTick) == MH_OK && MH_EnableHook((void*)b.clockTick) == MH_OK)
        logf("the engine's clock tick at rva 0x%llx", (unsigned long long)(b.clockTick - (const uint8_t*)exe));
    else logf("the engine's clock tick was not found as expected: in a witch time the player's systems get their clock once per update");
    return true;
}

// Nothing of ours is in the game's code: leave the process, so Mod Settings Menu shows the mod as not
// loaded instead of "Running".
DWORD unloadSelf() {
    logf("unloading this copy");
    WitchTime_Hooked = 0;
    if (g_instanceMutex) { CloseHandle(g_instanceMutex); g_instanceMutex = nullptr; }
    FreeLibraryAndExitThread(g_self, 0);
    return 0;
}

DWORD WINAPI initThread(LPVOID) {
    // The files' times first, then their contents: a change made while the mod starts is not missed.
    FILETIME iniT, menuT; fileTime(iniPath(), iniT); fileTime(menuPath(), menuT);
    loadSettings();
    logSettings();

    // One copy per process: a second one stands down.
    wchar_t name[64]; swprintf_s(name, L"WitchTime_Instance_%lu", GetCurrentProcessId());
    g_instanceMutex = CreateMutexW(nullptr, TRUE, name);
    if (g_instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        logf("inactive: another copy of WitchTime is already in charge");
        return unloadSelf();
    }

    std::wstring problem;
    g_hooked = installHook(problem);
    if (!g_hooked) {
        logf("WitchTime inactive: the game runs unmodified");
        showMessage(problem);
        return unloadSelf();
    }
    WitchTime_Hooked = 1;
    logf("active: WitchTime %s%s", kVersion, g_cfg.enabled ? "" : " (Enabled is off: a perfect dodge is the game's)");

    // Settings are re-read when either file changes, so they can be tuned while the game runs.
    for (;;) {
        Sleep(500);
        FILETIME a, b; fileTime(iniPath(), a); fileTime(menuPath(), b);
        if (CompareFileTime(&a, &iniT) != 0 || CompareFileTime(&b, &menuT) != 0) {
            iniT = a; menuT = b;
            loadSettings();
            logSettings();
        }
    }
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        wchar_t path[MAX_PATH]; GetModuleFileNameW(inst, path, MAX_PATH);
        g_dir = path; size_t p = g_dir.find_last_of(L"\\/"); g_dir = g_dir.substr(0, p + 1);
        InitializeCriticalSection(&g_logLock);
        g_t0 = GetTickCount();
        g_log = _wfsopen((g_dir + L"witchtime.log").c_str(), L"w", _SH_DENYWR);   // readable while the game runs
        logf("WitchTime %s loading from %ls", kVersion, g_dir.c_str());
        CreateThread(nullptr, 0, initThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (!reserved && g_hooked) { MH_DisableHook(MH_ALL_HOOKS); MH_Uninitialize(); g_hooked = false; }
        logf(reserved ? "game exiting" : "unloaded");
        if (g_log) { fclose(g_log); g_log = nullptr; }
    }
    return TRUE;
}
