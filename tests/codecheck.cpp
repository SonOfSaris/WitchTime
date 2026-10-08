// Checks the mod's code (player.h, bullet.h, patterns.h) against the GAME'S OWN code.
//
// The game's exe is mapped into this process as plain memory (sections copied, relocations applied; nothing
// of it is started). Then:
//   1. the mod's patterns are replayed (the seven functions of the player's time step, the entity lookup
//      inside the melee update);
//   2. a small world is built the way the game's storage is laid out (entity tables, two archetypes with their
//      chunks and component tables) and the game's real heron::hit_stop::update_timescales, both forms, and
//      coregame::local_time_scale::update, both systems in both forms, run on it. Where the game wrote the
//      time steps is where player::timeStepOf has to find them; then the mod's multiplication runs as the
//      hooks do it;
//   3. the witch time: the mod finds the request for a timescale curve, the update of the running curves,
//      the engine's clock update and tick, and runs each of them here on made-up data; the shape of a witch
//      time, the mod's entries in the game's list, the world's table of environments, the scheduler's systems
//      (a walk over made-up lists, and over every system name of this build), the table of the player's
//      systems against the build's own function signatures.
// The melee update itself is not run here: it needs half the player. Its part is watched in game (the log).
// usage: codecheck.exe [path to CONTROLResonant.exe]
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>
#include "../player.h"
#include "../bullet.h"
#include "../patterns.h"

using namespace player;

static int g_fail = 0, g_checks = 0;
static void check(bool ok, const char* what) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what); g_checks++; if (!ok) g_fail++; }
static bool same(float a, float b) { return fabsf(a - b) < 1e-6f; }

// ---------------------------------------------------------------- the game's image
static uint8_t* g_img = nullptr;
static uint8_t* g_text = nullptr; static size_t g_textSize = 0;
static uint32_t g_stamp = 0;

static bool mapImage(const wchar_t* path) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size; GetFileSizeEx(f, &size);
    std::vector<uint8_t> file((size_t)size.QuadPart);
    DWORD got = 0; size_t done = 0;
    while (done < file.size() && ReadFile(f, file.data() + done, (DWORD)std::min<size_t>(file.size() - done, 1 << 28), &got, nullptr) && got) done += got;
    CloseHandle(f);
    if (done != file.size()) return false;
    auto* dos = (IMAGE_DOS_HEADER*)file.data();
    auto* nt = (IMAGE_NT_HEADERS64*)(file.data() + dos->e_lfanew);
    g_stamp = nt->FileHeader.TimeDateStamp;
    g_img = (uint8_t*)VirtualAlloc(nullptr, nt->OptionalHeader.SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_img) return false;
    memcpy(g_img, file.data(), nt->OptionalHeader.SizeOfHeaders);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        size_t n = std::min(sec[i].SizeOfRawData, sec[i].Misc.VirtualSize);
        memcpy(g_img + sec[i].VirtualAddress, file.data() + sec[i].PointerToRawData, n);
        if (memcmp(sec[i].Name, ".text", 5) == 0) { g_text = g_img + sec[i].VirtualAddress; g_textSize = sec[i].Misc.VirtualSize; }
    }
    int64_t delta = (int64_t)((uintptr_t)g_img - nt->OptionalHeader.ImageBase);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    for (uint8_t* p = g_img + dir.VirtualAddress, *end = p + dir.Size; p < end;) {
        auto* blk = (IMAGE_BASE_RELOCATION*)p;
        if (!blk->SizeOfBlock) break;
        unsigned count = (blk->SizeOfBlock - sizeof(*blk)) / 2;
        auto* e = (uint16_t*)(blk + 1);
        for (unsigned i = 0; i < count; i++)
            if ((e[i] >> 12) == IMAGE_REL_BASED_DIR64) *(uint64_t*)(g_img + blk->VirtualAddress + (e[i] & 0xfff)) += delta;
        p += blk->SizeOfBlock;
    }
    // The game's unwind tables and the C++ runtime's frame handlers they name (imports of VCRUNTIME*.dll):
    // without them a fault inside the game's code cannot be unwound to a handler out here.
    auto& ex = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (ex.Size) RtlAddFunctionTable((PRUNTIME_FUNCTION)(g_img + ex.VirtualAddress), ex.Size / sizeof(RUNTIME_FUNCTION), (DWORD64)g_img);
    auto& im = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* d = (IMAGE_IMPORT_DESCRIPTOR*)(g_img + im.VirtualAddress); im.Size && d->Name; d++) {
        const char* dll = (const char*)g_img + d->Name;
        if (_strnicmp(dll, "VCRUNTIME", 9) != 0) continue;
        HMODULE m = LoadLibraryA(dll);
        auto* names = (IMAGE_THUNK_DATA64*)(g_img + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto* slots = (IMAGE_THUNK_DATA64*)(g_img + d->FirstThunk);
        for (; m && names->u1.AddressOfData; names++, slots++)
            if (!(names->u1.Ordinal & IMAGE_ORDINAL_FLAG64))
                if (FARPROC fn = GetProcAddress(m, ((IMAGE_IMPORT_BY_NAME*)(g_img + names->u1.AddressOfData))->Name)) slots->u1.Function = (ULONGLONG)fn;
    }
    return g_text != nullptr;
}

using StepOne = void (*)(uint8_t** world, uint64_t archetype, void* ctx);
using StepAll = void (*)(uint8_t** world, void* a2, void* ctx);

static bool g_showFaults = false;
static LONG CALLBACK showFault(EXCEPTION_POINTERS* x) {
    if (g_showFaults && x->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uint8_t* ip = (uint8_t*)x->ExceptionRecord->ExceptionAddress;
        printf("      fault: ip %p (image +0x%llx), touching %p, rsp %p, [rsp] image +0x%llx\n", ip, (unsigned long long)(ip - g_img), (void*)x->ExceptionRecord->ExceptionInformation[1],
               (void*)x->ContextRecord->Rsp, (unsigned long long)(*(uint8_t**)x->ContextRecord->Rsp - g_img));
        if ((uintptr_t)ip < 0x10000000) { printf("      (an import that is not resolved here: %.60s)\n", (const char*)g_img + (uintptr_t)ip + 2); g_showFaults = false; }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
// the game's request for a timescale curve, its update of the running curves and the engine's clock update
using StartCurve = int (*)(uint8_t* db, uint32_t curve, float duration);
using Timescales = void (*)(uint8_t* db, uint8_t* out);
using ClockUpdate = void (*)(uint8_t* fixed, uint8_t* variable, const uint8_t** source, const uint8_t* limits, const uint8_t* scaling);
static bool callStartCurve(StartCurve fn, uint8_t* db, uint32_t curve, float duration, int* id) {
    __try { *id = fn(db, curve, duration); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool callTimescales(Timescales fn, uint8_t* db, uint8_t* out) {
    __try { fn(db, out); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
using ClockTick = void (*)(uint8_t* fixed);
static bool callClockTick(ClockTick fn, uint8_t* fixed) {
    __try { fn(fixed); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool callClockUpdate(ClockUpdate fn, uint8_t* fixed, uint8_t* variable, const uint8_t** source, const uint8_t* limits, const uint8_t* scaling) {
    __try { fn(fixed, variable, source, limits, scaling); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    AddVectoredExceptionHandler(1, showFault);            // (says where the game's code faulted when g_showFaults is on)            // the game's code runs in here: nothing may be lost if it takes the process down
    const wchar_t* path = argc > 1 ? argv[1] : L"M:\\CONTROL Resonant\\CONTROLResonant.exe";
    if (!mapImage(path)) { printf("cannot map %ls\n", path); return 2; }
    printf("game exe timestamp %08X\n\n", g_stamp);

    // ---- 1. what the mod looks for
    pat::StepCode code; const char* why = "";
    bool foundAll = pat::findSteps(g_text, g_textSize, code, why);
    { char line[200]; sprintf_s(line, "the mod's search finds all seven functions%s%s", foundAll ? "" : ", missing: ", foundAll ? "" : why); check(foundAll, line); }
    if (!foundAll) { printf("\n%d of %d checks failed\n", g_fail, g_checks); return 1; }
    const uint8_t* melee = code.melee; const uint8_t* one = code.stepOne; const uint8_t* all = code.stepAll;
    printf("      melee update +0x%llx, hit stop time steps +0x%llx +0x%llx, engine time steps +0x%llx +0x%llx (all) +0x%llx +0x%llx (one)\n",
           (unsigned long long)(melee - g_img), (unsigned long long)(one - g_img), (unsigned long long)(all - g_img), (unsigned long long)(code.localAll[0] - g_img),
           (unsigned long long)(code.localAll[1] - g_img), (unsigned long long)(code.localOne[0] - g_img), (unsigned long long)(code.localOne[1] - g_img));
    check(pat::callTarget(code.localAll[0] + pat::kLocalAllBodyCall) != pat::callTarget(code.localAll[1] + pat::kLocalAllBodyCall),
          "the engine's fixed and variable update call two different bodies");
    check(melee[0] == 0x4C && melee[1] == 0x8B && melee[2] == 0xDC, "the melee update starts with mov r11, rsp (a start MinHook can move)");


    // ---- 2. the game's update_timescales on a world laid out like the game's
    {
        std::vector<uint8_t> worldMem(0x60000, 0);
        uint8_t* world = worldMem.data();
        const uint64_t player = (3ull << 32) | 7;                               // index 7, generation 3
        const uint64_t others[] = {(1ull << 32) | 2, (9ull << 32) | 5, (2ull << 32) | 11, (4ull << 32) | 12, (1ull << 32) | 13};
        std::vector<uint64_t> generations(64, 0), locations(64, 0xffff);
        auto put = [&](uint64_t e, uint16_t arch) { generations[(uint32_t)e] = e >> 32; locations[(uint32_t)e] = 0xABCD0000ull | arch; };
        uint64_t entityCount = 64; memcpy(world + kWorldEntityCount, &entityCount, 8);
        uint64_t* gp = generations.data(); uint64_t* lp = locations.data();
        memcpy(world + kWorldGenerations, &gp, 8); memcpy(world + kWorldLocations, &lp, 8);

        // archetype 5: three characters, the player in the middle; the time step is the second of three components
        // archetype 9: three more; the time step is the tenth of twelve (the game's lookup then compares 8 at a time)
        const uint32_t kRatios = 0x1e080f43;                                     // coregame::component::LocalTimeScaleRatios
        struct Archetype { uint16_t id; std::vector<uint8_t> chunk; std::vector<uint32_t> names, offsets; std::vector<uint64_t> ents; uint32_t stepOffset; };
        Archetype arch[2];
        arch[0].id = 5; arch[0].ents = {others[0], player, others[1]}; arch[0].names = {0x11111111, kTimeStepComponent, kRatios}; arch[0].offsets = {0x100, 0x200, 0x300}; arch[0].stepOffset = 0x200;
        arch[1].id = 9; arch[1].ents = {others[2], others[3], others[4]};
        for (uint32_t i = 0; i < 12; i++) { arch[1].names.push_back(i == 9 ? kTimeStepComponent : i == 3 ? kRatios : 0x40000000 + i); arch[1].offsets.push_back(0x80 + 0x40 * i); }
        arch[1].stepOffset = 0x80 + 0x40 * 9;
        for (Archetype& a : arch) {
            a.chunk.assign(0x1000, 0xEE);
            for (size_t i = 0; i < a.ents.size(); i++) { memcpy(a.chunk.data() + kChunkEntities + i * 8, &a.ents[i], 8); put(a.ents[i], a.id); }
            uint8_t* c = a.chunk.data(); uint32_t n = (uint32_t)a.ents.size();
            memcpy(world + kWorldChunks + a.id * 8, &c, 8); memcpy(world + kWorldCounts + a.id * 4, &n, 4);
            uint8_t* table = world + kWorldTables + a.id * kTableSize;
            uint32_t* np = a.names.data(); uint32_t* op = a.offsets.data(); uint32_t nn = (uint32_t)a.names.size();
            memcpy(table + kTableNames, &np, 8); memcpy(table + kTableOffsets, &op, 8); memcpy(table + kTableCount, &nn, 4);
        }
        // what the system gets: the frame's fixed and variable step, and its query (which archetypes it covers)
        float fixedStep = 1.0f / 60, variableStep = 0.0123f;
        void* env[3] = {nullptr, &fixedStep, &variableStep};
        std::vector<uint8_t> ctx(0x200, 0);
        void** envp = env; memcpy(ctx.data() + 0x58, &envp, 8);
        std::vector<uint64_t> bits(0x400 / 8 * 2, 0);                            // 400h bytes of archetype bits per query
        bits[0] = (1ull << 5) | (1ull << 9);
        uint64_t* bp = bits.data(); memcpy(world + 0x58480, &bp, 8);
        uint64_t archetypeCount[2] = {0, 16}; uint64_t* ac = archetypeCount; memcpy(world + 0x58478, &ac, 8);

        auto stepAt = [&](Archetype& a, size_t i) { return (float*)(a.chunk.data() + a.stepOffset + i * 8); };
        auto clear = [&] { for (Archetype& a : arch) for (size_t i = 0; i < a.ents.size(); i++) { stepAt(a, i)[0] = -1; stepAt(a, i)[1] = -1; } };
        auto allPlain = [&](Archetype& a) { bool ok = true; for (size_t i = 0; i < a.ents.size(); i++) ok &= stepAt(a, i)[0] == fixedStep && stepAt(a, i)[1] == variableStep; return ok; };

        clear();
        ((StepOne)one)(&world, 5, ctx.data());
        check(allPlain(arch[0]), "game, one archetype: every character of archetype 5 got {fixed, variable} where the test put its time step");
        check(stepAt(arch[1], 0)[0] == -1, "game, one archetype: archetype 9 was not written");
        ((StepOne)one)(&world, 9, ctx.data());
        check(allPlain(arch[1]), "game, one archetype: the same for archetype 9 (a component table of twelve)");
        clear();
        ((StepAll)all)(&world, nullptr, ctx.data());
        check(allPlain(arch[0]) && allPlain(arch[1]), "game, all archetypes: every character of both archetypes got {fixed, variable}");

        // the mod's way to the player's time step
        check(timeStepOf(world, player, -1) == stepAt(arch[0], 1), "mod: the player's time step is found where the game wrote it");
        check(timeStepOf(world, player, 5) == stepAt(arch[0], 1) && timeStepOf(world, player, 9) == nullptr, "mod: only in the archetype the player is in");
        check(timeStepOf(world, others[3], -1) == stepAt(arch[1], 1), "mod: a character of the other archetype too");
        check(timeStepOf(world, (4ull << 32) | 7, -1) == nullptr, "mod: an old handle of the same index (other generation) finds nothing");
        check(timeStepOf(world, (1ull << 32) | 63, -1) == nullptr && timeStepOf(world, (1ull << 32) | 64, -1) == nullptr && timeStepOf(world, ~0ull, -1) == nullptr,
              "mod: an entity without a place, an index past the table and the null handle find nothing");
        uint32_t zero = 0; uint32_t keep = arch[0].offsets[1];
        arch[0].offsets[1] = zero;
        check(timeStepOf(world, player, -1) == nullptr, "mod: no time step component in the archetype, nothing");
        arch[0].offsets[1] = keep;

        // the hooks' multiplication, frame by frame
        Applied ap;
        float* ps = timeStepOf(world, player, -1);
        clear(); ((StepOne)one)(&world, 5, ctx.data());
        check(apply(ps, 1.25f, ap) && same(ps[0], fixedStep * 1.25f) && same(ps[1], variableStep * 1.25f), "x1.25: the player's fixed and variable step are multiplied");
        check(stepAt(arch[0], 0)[0] == fixedStep && stepAt(arch[0], 2)[1] == variableStep, "x1.25: the neighbours in the chunk are untouched");
        check(!apply(ps, 1.25f, ap) && same(ps[0], fixedStep * 1.25f), "a second run in the same frame (the other form of the system) does not multiply again");
        check(same(baked(ps, ap), 1.25f), "the melee update is told x1.25 is in the step");
        ((StepAll)all)(&world, nullptr, ctx.data());                             // next frame: the game writes the plain step again
        check(same(baked(ps, ap), 1.0f), "next frame, before the hook: the game's plain step is there, factor 1");
        check(apply(ps, 1.6f, ap) && same(ps[0], fixedStep * 1.6f) && same(baked(ps, ap), 1.6f), "next frame with another factor (recovery x1.6): applied once, from the plain step");
        ((StepAll)all)(&world, nullptr, ctx.data());
        check(!apply(ps, 1.0f, ap) && ps[0] == fixedStep && same(baked(ps, ap), 1.0f), "factor 1: nothing is written");
        apply(ps, 1.6f, ap);
        ps[0] = fixedStep * 0.05f; ps[1] = variableStep * 0.05f;                 // a hit stop overwrites the step after the hook
        check(same(baked(ps, ap), 1.0f), "hit stop: the game's own value is in the step, the melee update is told factor 1");
        variableStep = 0.0200f;
        ((StepAll)all)(&world, nullptr, ctx.data());
        check(apply(ps, 1.6f, ap) && same(ps[1], 0.0200f * 1.6f), "a frame of another length: the new variable step is multiplied");

        // the engine's own update: a blend of two clocks by the character's ratios {fixed, variable}
        auto ratioAt = [&](Archetype& a, size_t i) { return (float*)(a.chunk.data() + (a.id == 5 ? 0x300 : 0x80 + 0x40 * 3) + i * 8); };
        for (Archetype& a : arch) for (size_t i = 0; i < a.ents.size(); i++) { ratioAt(a, i)[0] = 1; ratioAt(a, i)[1] = 1; }
        ratioAt(arch[0], 1)[0] = 0.25f; ratioAt(arch[0], 1)[1] = 0.5f;          // the player: a quarter / half of the second clock
        float clocks[2] = {1.0f / 30, 1.0f / 60};
        void* envL[2] = {nullptr, clocks};
        std::vector<uint8_t> ctxL(0x200, 0);
        void** envLp = envL; memcpy(ctxL.data() + 0x58, &envLp, 8);
        const float blendFixed = 0.75f * clocks[0] + 0.25f * clocks[1], blendVariable = 0.5f * clocks[0] + 0.5f * clocks[1];
        for (int form = 0; form < 2; form++) {                                   // 0 = all archetypes, 1 = one archetype
            clear();
            if (form == 0) ((StepAll)code.localAll[0])(&world, nullptr, ctxL.data());
            else { ((StepOne)code.localOne[0])(&world, 5, ctxL.data()); ((StepOne)code.localOne[0])(&world, 9, ctxL.data()); }
            bool fixedOk = same(stepAt(arch[0], 1)[0], blendFixed) && same(stepAt(arch[0], 0)[0], clocks[1]) && same(stepAt(arch[1], 2)[0], clocks[1]);
            bool otherPart = stepAt(arch[0], 1)[1] == -1 && stepAt(arch[1], 0)[1] == -1;
            if (form == 0) ((StepAll)code.localAll[1])(&world, nullptr, ctxL.data());
            else { ((StepOne)code.localOne[1])(&world, 5, ctxL.data()); ((StepOne)code.localOne[1])(&world, 9, ctxL.data()); }
            bool variableOk = same(stepAt(arch[0], 1)[1], blendVariable) && same(stepAt(arch[0], 2)[1], clocks[1]) && same(stepAt(arch[1], 1)[1], clocks[1]) && same(stepAt(arch[0], 1)[0], blendFixed);
            check(fixedOk && otherPart && variableOk, form == 0 ? "game, engine update (all archetypes): the first of the pair writes the fixed step, the second the variable step, as a blend by the ratios"
                                                                : "game, engine update (one archetype): the same");
        }
        // a frame as the hooks see it: the hit stop system writes, then the engine's system writes again
        Applied ap2; clear();
        ((StepAll)all)(&world, nullptr, ctx.data());
        apply(ps, 1.25f, ap2);
        check(same(ps[0], fixedStep * 1.25f) && same(ps[1], variableStep * 1.25f), "frame: after the hit stop system's write both parts are x1.25");
        ((StepAll)code.localAll[0])(&world, nullptr, ctxL.data());
        check(same(ps[0], blendFixed) && same(baked(ps, ap2), 1.0f), "frame: the engine's fixed update overwrites the fixed part");
        check(apply(ps, 1.25f, ap2) && same(ps[0], blendFixed * 1.25f) && same(ps[1], variableStep * 1.25f) && same(baked(ps, ap2), 1.25f),
              "frame: the hook after it multiplies the new fixed part once and leaves the variable part (already done) alone");
        ((StepOne)code.localOne[1])(&world, 5, ctxL.data());
        check(apply(ps, 1.25f, ap2) && same(ps[1], blendVariable * 1.25f) && same(ps[0], blendFixed * 1.25f), "frame: the same for the engine's variable update");
        check(!apply(ps, 1.25f, ap2), "frame: nothing left to do");
    }


    // ---- 3. the witch time
    {
        using namespace bullet;
        auto rel = [](float a, float b) { return fabsf(a - b) <= 1e-5f * fabsf(b); };
        auto isKnown = [](const char* name) { return known(name, strlen(name)) != nullptr; };
        pat::BulletCode bc; pat::findBullet(g_text, g_textSize, bc);
        check(bc.startCurve != nullptr, "the mod finds the game's request for a timescale curve");
        check(bc.timescales != nullptr, "and the update of the running curves, with the loads, the store of a curve's value and the store of the world's scale it relies on inside it");
        check(bc.layout, "the scheduler's call of a system, a system's name, the world's table of environments and the engine's clock update are each in the game's code once");
        if (bc.startCurve && bc.timescales) {
            printf("      timescale curve request +0x%llx, update +0x%llx\n", (unsigned long long)(bc.startCurve - g_img), (unsigned long long)(bc.timescales - g_img));
            const uint8_t s0[6] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x40}, t0[5] = {0x48, 0x89, 0x5C, 0x24, 0x10};
            check(memcmp(bc.startCurve, s0, 6) == 0 && memcmp(bc.timescales, t0, 5) == 0, "both start with instructions MinHook can move");
            int callers = 0;
            for (size_t i = 0; i + 5 <= g_textSize; i++) if (g_text[i] == 0xE8 && pat::callTarget(g_text + i) == bc.startCurve) callers++;
            check(callers == 1, "one place in the game's code calls the request: the perfect dodge (so every call is one)");
        }

        // the game's own request, run here: what a request is and where the id comes from
        if (bc.startCurve) {
#pragma pack(push, 1)
            struct Request { uint32_t curve; int index; int id; float duration; uint8_t end; uint8_t pad[3]; };
#pragma pack(pop)
            static Request queue[8]; static uint8_t db[0x40];
            Request* q = queue; uint32_t cap = 8; int last = 40;
            memset(queue, 0xEE, sizeof queue);
            memcpy(db + 0x10, &q, 8); memcpy(db + 0x1c, &cap, 4); memcpy(db + 0x24, &last, 4);
            int id = -1, id2 = -1;
            const bool ran = callStartCurve((StartCurve)bc.startCurve, db, 0x5a6f684a, -1.0f, &id) && callStartCurve((StartCurve)bc.startCurve, db, 0x5a6f684a, 3.0f, &id2);
            check(ran && id == 41 && id2 == 42 && rd<int>(db, 0x24) == 42 && rd<uint32_t>(db, 0x18) == 2, "the game's request runs here: ids count up from the database's last one (+24h), two requests are in its list (+10h)");
            check(ran && sizeof(Request) == 0x14 && queue[0].curve == 0x5a6f684a && queue[0].index == -1 && queue[0].id == 41 && queue[0].duration == -1 && queue[0].end == 0 &&
                  queue[1].id == 42 && queue[1].duration == 3 && queue[1].curve == 0x5a6f684a,
                  "a request is {curve, -1, id, duration, 0}, 14h bytes: the perfect dodge's own with duration -1, a second one with the mod's 3 s");
        }
        // the game's update of the running curves, run here with nothing running
        if (bc.timescales) {
            static uint8_t db[0x40], out[0x10];
            float two = 2; memcpy(db + kDbLowest, &two, 4); memcpy(out + kWorldScale, &two, 4); db[0x28] = 1;
            bool ran = callTimescales((Timescales)bc.timescales, db, out);
            check(ran && rd<float>(db, kDbLowest) == 1 && rd<float>(out, kWorldScale) == 1 && db[0x28] == 0, "the game's update runs here with no curve running: the lowest value (db +20h) and the world's scale (+8) are 1");
            memcpy(out + kWorldScale, &two, 4);
            ran = ran && callTimescales((Timescales)bc.timescales, db, out);
            check(ran && rd<float>(out, kWorldScale) == 2, "and when nothing was running before either it leaves the world's scale alone");
        }
        // the engine's clock update, run here: what FixedTime and VariableTime hold
        {
            int count = 0; const size_t o = pat::find(g_text, g_textSize, pat::kClockUpdate, count);
            const uint8_t* f = count == 1 && o >= 0xb ? g_text + o - 0xb : nullptr;
            const uint8_t f0[3] = {0x49, 0x8B, 0x00};
            check(f && memcmp(f, f0, 3) == 0, "the engine's clock update is found (the function starts 0Bh before the mod's pattern)");
            if (f && memcmp(f, f0, 3) == 0) {
                static uint8_t fixed[kFixedSize], variable[kVariableSize], source[0x40], limits[0x20], scaling[0x10];
                const uint8_t* sp = source; float frame = 0.02f, tick = 1.0f / 60, most = 0.1f, scale = 0.25f;
                memcpy(source, &frame, 4); memcpy(source + 0x14, &tick, 4); memcpy(limits + 0x10, &most, 4); memcpy(scaling + 4, &scale, 4);
                const bool ran = callClockUpdate((ClockUpdate)f, fixed, variable, &sp, limits, scaling);
                check(ran && rd<float>(fixed, kTimeStep) == tick * 0.25f && rd<float>(fixed, kTimeUnscaled) == tick && rel(rd<float>(fixed, kTimeRate), 240),
                      "FixedTime with the time scale at 0.25: +0 the step (a quarter of 1/60 s), +4 the unscaled step, +8 one over the step");
                check(ran && rd<float>(variable, kTimeStep) == frame * 0.25f && rd<float>(variable, kTimeUnscaled) == frame && rel(rd<float>(variable, kTimeRate), 200), "VariableTime: the same three for the frame's time");
                // the mod's copy for the player's systems
                Clocks c;
                check(same(factor(fixed, 1, 1), 4) && factor(fixed, 0, 1) == 1 && same(factor(fixed, 0.5f, 1), 2.5f) && same(factor(fixed, 1, 1.5f), 6) && factor(nullptr, 1, 1) == 1,
                      "the factor from the world's step to the player's: x4 with the clock free, x1 with the world, in between while it comes free; x6 for a player speed of 150%");
                double now = 123.5; uint64_t ticks = 77; memcpy(fixed + 0x10, &now, 8); memcpy(fixed + 0x18, &ticks, 8); fixed[0x20] = 1; memcpy(variable + 0x10, &now, 8);
                refresh(c, fixed, variable, 1, 1);
                check(same(rd<float>(c.fixed, kTimeStep), tick) && rel(rd<float>(c.fixed, kTimeRate), 60) && rd<float>(c.fixed, kTimeUnscaled) == tick && rd<double>(c.fixed, 0x10) == 123.5 &&
                      rd<uint64_t>(c.fixed, 0x18) == 77 && c.fixed[0x20] == 1, "the copy of FixedTime: the full step and one over it; the unscaled step, the time, the ticks and the flag as the game has them");
                check(same(rd<float>(c.variable, kTimeStep), frame) && rel(rd<float>(c.variable, kTimeRate), 50) && rd<double>(c.variable, 0x10) == 123.5, "the copy of VariableTime the same way");
                refresh(c, fixed, variable, 0, 1);
                check(memcmp(c.fixed, fixed, kFixedSize) == 0 && memcmp(c.variable, variable, kVariableSize) == 0, "with the player's clock not free the copies are the game's clocks, byte for byte");
                float zero = 0; memcpy(fixed + kTimeStep, &zero, 4);
                check(factor(fixed, 1, 1) == 1, "a clock that stands still (step 0): factor 1");
            }
        }

        // the engine's clock tick, run here
        check(bc.clockTick != nullptr, "the mod finds the engine's clock tick");
        if (bc.clockTick) {
            static uint8_t fixed[kFixedSize];
            float step = 0.004f, plain = 1.0f / 60; double now = 10; uint64_t ticks = 5;
            memcpy(fixed + kTimeStep, &step, 4); memcpy(fixed + kTimeUnscaled, &plain, 4); memcpy(fixed + 0x10, &now, 8); memcpy(fixed + 0x18, &ticks, 8);
            const uint8_t c0[4] = {0x80, 0x79, 0x20, 0x00};
            bool ran = memcmp(bc.clockTick, c0, 4) == 0 && callClockTick((ClockTick)bc.clockTick, fixed);
            check(ran && rd<uint64_t>(fixed, 0x18) == 6 && rd<double>(fixed, 0x10) == 10 + (double)step, "the game's tick runs here: FixedTime's tick count (+18h) goes up by one, its time (+10h) by the step");
            fixed[0x20] = 1;
            ran = ran && callClockTick((ClockTick)bc.clockTick, fixed);
            check(ran && rd<uint64_t>(fixed, 0x18) == 7 && rd<double>(fixed, 0x10) == 10 + (double)step + (double)plain, "with the flag at +20h set the time goes on by the unscaled step");
        }

        // the shape of a bullet time: 3 s, the game's slow motion 0.6 s, the world at 20%
        {
            Shape s = shape(0.3f, 3, 0.6f, 0.2f, -1);
            check(s.scale == 0.2f && s.unslow == 0 && s.hold == 1, "0.3 s in: the mod asks for 20% (the game's own curve is lower and counts), the player's clock stays with the world");
            s = shape(0.6f, 3, 0.6f, 0.2f, 0.1f);
            check(same(s.scale, 0.1f) && s.unslow == 0, "at 0.6 s, when the game's curve has ended on 10%: the mod's value starts there");
            s = shape(0.725f, 3, 0.6f, 0.2f, 0.1f);
            check(same(s.scale, 0.15f) && s.unslow > 0.6f && s.unslow < 0.8f, "an eighth of a second later: halfway to 20%, the player's clock most of the way free");
            s = shape(0.85f, 3, 0.6f, 0.2f, 0.1f);
            check(s.scale == 0.2f && s.unslow == 1 && s.hold == 1, "at 0.85 s: 20%, the player's clock free");
            s = shape(0.7f, 3, 0.6f, 0.05f, 0.1f);
            check(s.scale == 0.05f, "a setting below where the game's curve ended: no way up, the setting");
            s = shape(2.65f, 3, 0.6f, 0.2f, 0.1f);
            check(s.scale == 0.2f && s.hold == 1 && s.unslow == 1, "it holds until 2.65 s (the world at the very number of the setting)");
            s = shape(2.825f, 3, 0.6f, 0.2f, 0.1f);
            check(same(s.scale, 0.6f) && same(s.hold, 0.5f) && s.unslow == 1, "halfway through the last 0.35 s: the world at 60%, the player's clock still its own");
            s = shape(3, 3, 0.6f, 0.2f, 0.1f);
            check(s.scale == 1 && s.unslow == 0 && s.hold == 0, "at 3 s it is over");
            s = shape(0.8f, 1, 0.6f, 0.2f, 0.1f);
            check(s.hold == 1 && same(s.unslow, 1) && same(shape(0.9f, 1, 0.6f, 0.2f, 0.1f).hold, 0.5f), "a bullet time of 1 s: the way back takes half of what is left behind the game's slow motion");
            bullet::Config c; c.player = 1.5f;
            check(same(target(c, shape(1, 3, 0.6f, 0.2f, 0.1f)), 1.5f) && same(target(c, shape(2.825f, 3, 0.6f, 0.2f, 0.1f)), 1.25f) && target(c, shape(3, 3, 0.6f, 0.2f, 0.1f)) == 1,
                  "the player's speed aimed at: the setting while it holds, back to 1 with the world");
        }

        // the mod's entries in the game's list of running curves
        {
#pragma pack(push, 1)
            struct Item { int index; uint32_t pad0; uint64_t resource; const uint8_t* curve; uint32_t generation, lock; int id; float duration, elapsed, value; };
#pragma pack(pop)
            static Item items[4]; static uint8_t db[0x40], out[0x10], curve[0x20];
            Item* ip = items; float length = 0.6f; memcpy(curve + kCurveLength, &length, 4); memcpy(db + kDbActive, &ip, 8);
            auto count = [&](uint32_t n) { memcpy(db + kDbActiveCount, &n, 4); };
            bullet::Config c; c.on = true;
            Runs runs;
            count(0);
            Now now = after(c, db, out, runs);
            check(!now.active && !now.started && !now.ended, "no bullet time asked for: nothing to do");
            runs.add(7, 6);
            now = after(c, db, out, runs);
            check(!now.active && runs.n == 1 && runs.r[0].waited == 1, "asked for, not in the game's list yet: the mod waits");
            items[0] = Item{0, 0, 1, curve, 0, 0, 6, -1, 0.3f, 0.19f}; items[1] = Item{0, 0, 1, curve, 0, 0, 7, 3, 0.3f, 0.07f}; items[2] = Item{1, 0, 1, curve, 0, 0, 99, -1, 0.1f, 0.5f};
            count(3);
            now = after(c, db, out, runs);
            check(now.active && now.started && !now.ended && now.shape.unslow == 0 && now.t == 0.3f && now.total == 3 && now.game == 0.6f && items[1].value == 0.2f && items[0].value == 0.19f && items[2].value == 0.5f,
                  "both in the list, 0.3 s in: the mod's entry gets 20%, the game's own and a third one keep their values");
            check(rd<float>(db, kDbLowest) == 0.19f && rd<float>(out, kWorldScale) == 0.19f && runs.r[0].gameEnd == 0.19f, "the lowest value is the world's scale: the game's own curve");
            items[2].value = 0.05f;
            now = after(c, db, out, runs);
            check(rd<float>(out, kWorldScale) == 0.05f && !now.started, "another slow motion that is lower counts");
            items[0].value = 0.1f; items[0].elapsed = items[1].elapsed = 0.59f; items[1].value = 0.01f; count(2);
            now = after(c, db, out, runs);
            check(runs.r[0].gameEnd == 0.1f && rd<float>(out, kWorldScale) == 0.1f, "the game's curve near its end: 10%");
            items[0] = items[1]; items[0].elapsed = 0.7f; items[0].value = 0.02f; count(1);
            now = after(c, db, out, runs);
            check(now.active && same(now.shape.scale, 0.1f + 0.1f * smooth(0.4f)) && same(now.shape.unslow, smooth(0.5f)) && rd<float>(out, kWorldScale) == now.shape.scale && items[0].value == now.shape.scale,
                  "the game's own curve gone, 0.7 s in: the mod's entry alone sets the world's scale, on its way from 10% to 20%; the player's clock is coming free");
            items[0].elapsed = 1.5f;
            c.on = false;
            now = after(c, db, out, runs);
            check(now.active && now.shape.scale == 1 && now.shape.unslow == 0 && rd<float>(out, kWorldScale) == 1, "switched off while it runs: the entry no longer slows anything");
            c.on = true;
            runs.add(9, 8);
            items[1] = Item{0, 0, 1, curve, 0, 0, 8, -1, 0.1f, 0.04f}; items[2] = Item{0, 0, 1, curve, 0, 0, 9, 3, 0.1f, 0.9f}; count(3);
            now = after(c, db, out, runs);
            check(now.active && now.started && now.t == 0.1f && now.shape.unslow == 0 && items[0].value == 0.2f && items[2].value == 0.2f && rd<float>(out, kWorldScale) == 0.04f && runs.n == 2,
                  "a second perfect dodge during the first one's bullet time: the newest one says where the player's clock is (with the world again)");
            items[0] = items[2]; items[0].elapsed = 2.9f; count(1);
            now = after(c, db, out, runs);
            check(runs.n == 1 && runs.r[0].id == 9 && now.active && !now.ended && now.shape.hold < 0.3f, "the first one's entry is gone: the second goes on");
            count(0);
            now = after(c, db, out, runs);
            check(runs.n == 0 && now.ended && !now.active, "the last one gone: over");
            for (int i = 0; i < 6; i++) runs.add(20 + i, 0);
            check(runs.n == Runs::kRuns && runs.r[0].id == 22 && runs.r[3].id == 25, "more than four at once: the oldest give way");
            for (int i = 0; i <= Runs::kWait; i++) now = after(c, db, out, runs);
            check(runs.n == 0 && !now.ended, "requests that never show up in the game's list are forgotten after a while");
        }

        // the world's table of environments
        {
            static uint8_t world[0x60000], a[8], b[8], f[8];
            uint32_t ids[5] = {0x10000000, kVariableTime, kFixedTime, 0x7d003b9e, 0xfffffff0}; uint8_t* ptrs[5] = {a, b, f, a, a};
            uint32_t* ip = ids; uint8_t** pp = ptrs; uint32_t n = 5;
            memcpy(world + kWorldEnvIds, &ip, 8); memcpy(world + kWorldEnvCount, &n, 4); memcpy(world + kWorldEnvs, &pp, 8);
            check(envOf(world, kFixedTime) == f && envOf(world, kVariableTime) == b && envOf(world, 0x10000000) == a && envOf(world, 0xfffffff0) == a, "an environment is found by its id in the world's sorted table");
            check(envOf(world, 0x4c8470db) == nullptr && envOf(world, 1) == nullptr && envOf(world, 0xffffffff) == nullptr, "an id that is not in it: nothing");
            n = 0; memcpy(world + kWorldEnvCount, &n, 4);
            check(envOf(world, kFixedTime) == nullptr, "an empty table: nothing");
        }

        // the scheduler's systems
        {
            enum { N = 7 };
            static uint8_t systems[(N + 2) * kSysSize], fixed[kFixedSize], variable[kVariableSize], other[8];
            static uint8_t* envs[N][10];
            const char* names[N] = {"heron::encounter::update_encounter::system", "heron::jump::update_raising::system", "heron::tailcamera::update_camera_view::system",
                                    "heron::camera_input::calculate_rotation_delta::system", "heron::dodge::update_perfect_dodge_window::system", "heron::jump::update_raising::systemX",
                                    "heron::dash::update::system"};
            memset(systems, 0, sizeof systems); memset(envs, 0, sizeof envs);
            for (int i = 0; i < N; i++) {
                uint8_t* s = systems + i * kSysSize; uint16_t id = (uint16_t)i; uint64_t len = strlen(names[i]); uint8_t** e = envs[i];
                memcpy(s + kSysId, &id, 2); memcpy(s + kSysName, &names[i], 8); memcpy(s + kSysNameLength, &len, 8); memcpy(s + kSysEnvs, &e, 8);
            }
            { uint16_t id = 99; memcpy(systems + N * kSysSize + kSysId, &id, 2); }              // behind the last one: something else
            envs[0][1] = fixed; envs[1][2] = fixed; envs[2][7] = variable; envs[3][1] = variable; envs[3][2] = fixed; envs[4][8] = other; envs[5][2] = fixed; envs[6][1] = fixed;
            Clocks c; Slots slots;
            Swap w = engage(systems + 3 * kSysSize, fixed, variable, c, slots);
            check(w.systems == 7 && w.named == 5 && w.swapped == 5 && w.other == 1 && w.already == 0 && slots.n == 5,
                  "from any system of the array: 7 systems, 5 of them known to the mod, 5 clocks handed over (and noted), 1 slot left alone");
            check(envs[1][2] == c.fixed && envs[2][7] == c.variable && envs[3][1] == c.variable && envs[3][2] == c.fixed && envs[6][1] == c.fixed, "jump, camera view, rotation (both clocks) and Rush read the mod's copies");
            check(envs[0][1] == fixed && envs[4][8] == other && envs[5][2] == fixed, "a system the mod does not know, a slot with something else in it and a name that only starts like a known one keep what they have");
            { Slots more; w = engage(systems, fixed, variable, c, more); check(w.swapped == 0 && w.already == 5 && w.other == 1 && more.n == 0, "once more: nothing to do, nothing noted"); }
            check(release(slots, c) == 5 && slots.n == 0 && envs[1][2] == fixed && envs[2][7] == variable && envs[3][1] == variable && envs[3][2] == fixed && envs[6][1] == fixed && envs[4][8] == other,
                  "the way back, by the mod's own list: the game's clocks are in their slots again");
            check(release(slots, c) == 0, "back once more: nothing to do");
            w = engage(systems, nullptr, variable, c, slots);
            check(w.swapped == 2 && slots.n == 2 && envs[1][2] == fixed && envs[2][7] == c.variable, "without the game's fixed clock only the frame clocks change hands");
            release(slots, c);
            engage(systems, fixed, variable, c, slots); envs[6][1] = other;
            check(release(slots, c) == 4 && envs[6][1] == other && envs[1][2] == fixed && envs[3][1] == variable, "a slot that no longer holds the mod's copy on the way back is left as it is");
            envs[6][1] = fixed;
            { uint16_t id = 5; memcpy(systems + 2 * kSysSize + kSysId, &id, 2); }               // the array does not go on as expected
            w = engage(systems, fixed, variable, c, slots);
            check(w.systems == 2 && w.swapped == 1 && envs[2][7] == variable, "the walk stops where a system's id is not its place in the array");
            release(slots, c);
            { uint16_t id = 2; memcpy(systems + 2 * kSysSize + kSysId, &id, 2); }
            // memory the mod is told it may not read: the array from its fifth system on
            Readable ok = [](const void* p, size_t n) { const uint8_t* q = (const uint8_t*)p; return q + n <= systems + 4 * kSysSize || q >= systems + sizeof systems; };
            w = engage(systems + kSysSize, fixed, variable, c, slots, ok);
            check(w.systems == 4 && w.swapped == 4 && envs[6][1] == fixed && envs[3][2] == c.fixed, "the walk ends where the array cannot be read; what lies before is done");
            check(release(slots, c, ok) == 4 && envs[3][2] == fixed, "and comes back");
            check(engage(systems + 5 * kSysSize, fixed, variable, c, slots, ok).systems == 0 && slots.n == 0, "a system to start from that cannot be read: nothing");
            for (int i = 0; i < Slots::kMax + 8; i++) { static uint8_t* many[Slots::kMax + 8]; many[i] = fixed; Swap x; take(&many[i], fixed, c.fixed, slots, nullptr, x); if (i == Slots::kMax + 7) check(slots.n == Slots::kMax && many[i] == fixed && many[0] == c.fixed, "more slots than the list holds: the rest keep the game's clock"); }
            check(release(slots, c) == Slots::kMax, "(all of them back)");
            check(engage(nullptr, fixed, variable, c, slots).systems == 0, "no system to start from: nothing");
            // the plan: one walk, the bullet times start from what it found
            {
                Plan p; survey(systems + 3 * kSysSize, p);
                check(p.done && p.systems == 7 && p.named == 5 && p.n == 5 && p.shared == 0 && p.end == 1, "a survey notes the five systems of the table");
                w = engage(p, fixed, variable, c, slots);
                check(w.swapped == 5 && w.other == 1 && w.stale == 0 && w.named == 5 && w.systems == 7 && envs[1][2] == c.fixed, "the plan hands their clocks over without another walk");
                check(release(slots, c) == 5, "(back)");
                { uint16_t id = 7; memcpy(systems + 1 * kSysSize + kSysId, &id, 2); }
                uint8_t** was = envs[6]; uint8_t** moved = envs[0]; memcpy(systems + 6 * kSysSize + kSysEnvs, &moved, 8);
                w = engage(p, fixed, variable, c, slots);
                check(w.stale == 2 && w.named == 3 && w.swapped == 3 && envs[1][2] == fixed && envs[6][1] == fixed, "a system whose id or list of environments changed since the survey is stale: left alone, counted");
                check(release(slots, c) == 3, "(back)");
                { uint16_t id = 1; memcpy(systems + 1 * kSysSize + kSysId, &id, 2); }
                memcpy(systems + 6 * kSysSize + kSysEnvs, &was, 8);
                Plan none; w = engage(none, fixed, variable, c, slots);
                check(!none.done && w.swapped == 0 && w.named == 0 && w.systems == 0, "no plan: nothing");
            }
            // names the mod does not read are not the end of the list
            {
                static char longName[2774]; memset(longName, 'x', 2773); memcpy(longName, "coregame::movementmodel::update_pre_animation::system<class ecs::Entity<", 72);
                const char* was0 = names[0]; uint64_t wasLen0 = strlen(names[0]);
                const char* ln = longName; uint64_t len = 2773;
                memcpy(systems + 0 * kSysSize + kSysName, &ln, 8); memcpy(systems + 0 * kSysSize + kSysNameLength, &len, 8);      // the engine's template system
                const char* was5 = names[5]; uint64_t wasLen5 = strlen(names[5]);
                const char* none = nullptr; len = 0;
                memcpy(systems + 5 * kSysSize + kSysName, &none, 8); memcpy(systems + 5 * kSysSize + kSysNameLength, &len, 8);    // no name at all
                w = engage(systems + 6 * kSysSize, fixed, variable, c, slots);
                check(w.systems == 7 && w.unnamed == 2 && w.named == 5 && w.swapped == 5 && w.end == 1 && envs[0][1] == fixed && envs[5][2] == fixed,
                      "a system with a name of 2,773 characters and one without a name: passed over, the walk goes on to the end of the list");
                check(strcmp(w.first, "heron::jump::update_raising::system") == 0 && strcmp(w.last, "heron::dash::update::system") == 0, "the first and the last name read are kept for the log");
                check(release(slots, c) == 5, "(back)");
                const char* empty = ""; len = 0;
                memcpy(systems + 0 * kSysSize + kSysName, &empty, 8); memcpy(systems + 0 * kSysSize + kSysNameLength, &len, 8);   // an empty name
                const char* gone = (const char*)systems + 4 * kSysSize; len = 20;                                                 // a name in memory the mod may not read
                memcpy(systems + 5 * kSysSize + kSysName, &gone, 8); memcpy(systems + 5 * kSysSize + kSysNameLength, &len, 8);
                Readable some = [](const void* p, size_t n) { const uint8_t* q = (const uint8_t*)p; return n != 20 || q != systems + 4 * kSysSize; };
                w = engage(systems, fixed, variable, c, slots, some);
                check(w.systems == 7 && w.unnamed == 2 && w.swapped == 5, "an empty name and a name that cannot be read: the same");
                check(release(slots, c, some) == 5, "(back)");
                memcpy(systems + 0 * kSysSize + kSysName, &was0, 8); memcpy(systems + 0 * kSysSize + kSysNameLength, &wasLen0, 8);
                memcpy(systems + 5 * kSysSize + kSysName, &was5, 8); memcpy(systems + 5 * kSysSize + kSysNameLength, &wasLen5, 8);
            }
            // lists of environments that more than one system reads
            {
                uint8_t** list = envs[1]; memcpy(systems + 0 * kSysSize + kSysEnvs, &list, 8);        // the encounter system reads the jump system's list
                w = engage(systems, fixed, variable, c, slots);
                check(w.shared == 1 && w.swapped == 4 && w.other == 1 && envs[1][2] == fixed && envs[6][1] == c.fixed && envs[3][2] == c.fixed,
                      "a system whose list of environments is also read by a system the mod does not know keeps the game's clock; the others get the mod's");
                check(release(slots, c) == 4, "(back)");
                list = envs[0]; memcpy(systems + 0 * kSysSize + kSysEnvs, &list, 8);
                list = envs[3]; memcpy(systems + 1 * kSysSize + kSysEnvs, &list, 8);                  // jump and rotation, both in the table, read one list
                w = engage(systems, fixed, variable, c, slots);
                check(w.shared == 0 && w.swapped == 4 && w.already == 1 && envs[3][2] == c.fixed && envs[3][1] == c.variable && envs[1][2] == fixed,
                      "two systems of the table with one list: its clocks change hands once, both systems read the mod's copy");
                check(release(slots, c) == 4 && envs[3][2] == fixed && envs[3][1] == variable, "(back)");
                list = envs[1]; memcpy(systems + 1 * kSysSize + kSysEnvs, &list, 8);
            }
        }

        // the table of the player's systems against this build's own function signatures
        {
            // every system's name string in the game is its signature: "...getName<void __cdecl <name>(<parameters>)>(void)"
            const char marker[] = "ecs::system::getName<void __cdecl heron::";
            const size_t ml = sizeof marker - 1;
            auto* nt = (IMAGE_NT_HEADERS64*)(g_img + ((IMAGE_DOS_HEADER*)g_img)->e_lfanew);
            const size_t size = nt->OptionalHeader.SizeOfImage;
            std::vector<const char*> sigs;
            for (size_t i = 0; i + ml < size; i++) if (g_img[i] == 'e' && memcmp(g_img + i, marker, ml) == 0) sigs.push_back((const char*)g_img + i + ml - 7);     // at "heron::"
            auto clockAt = [](const char* params, const char* clock) {
                int depth = 0, index = 0, found = -1; const char* start = params; const size_t cl = strlen(clock);
                for (const char* p = params;; p++) {
                    if (*p == '<' || *p == '(') depth++;
                    if ((*p == ',' && depth == 0) || (*p == ')' && depth == 0) || !*p) {
                        bool entity = strncmp(start, "class ecs::Entity", 17) == 0, has = false;
                        for (const char* q = start; q + cl <= p; q++) if (memcmp(q, clock, cl) == 0) { has = true; break; }
                        if (has && !entity) found = found < 0 ? index : -2;
                        if (*p != ',') break;
                        index++; start = p + 1;
                    } else if (*p == '>' || *p == ')') depth--;
                }
                return found;
            };
            int foundAll = 0, right = 0; const char* firstWrong = nullptr;
            for (const SysTime& s : kSystems) {
                const size_t nl = strlen(s.name);
                for (const char* sig : sigs) {
                    if (strncmp(sig, s.name, nl) != 0 || sig[nl] != '(') continue;
                    foundAll++;
                    if (clockAt(sig + nl + 1, "coregame::global::FixedTime") == s.fixed && clockAt(sig + nl + 1, "coregame::global::VariableTime") == s.variable) right++;
                    else if (!firstWrong) firstWrong = s.name;
                    break;
                }
            }
            printf("      %d system signatures in the game, %d systems in the mod's table\n", (int)sigs.size(), kSystemCount);
            check(sigs.size() > 1000, "the game's system signatures are found (over a thousand)");
            check(foundAll == kSystemCount, "every system of the mod's table is a system of this build");
            if (firstWrong) printf("      first one that differs: %s\n", firstWrong);
            check(right == kSystemCount, "and each takes the game's clocks at the parameters the table names (FixedTime and VariableTime, read from the build's own signatures)");
            bool sane = kSystemCount == 59;
            for (const SysTime& s : kSystems) sane = sane && strncmp(s.name, "heron::", 7) == 0 && strlen(s.name) <= kMaxName && s.fixed < 16 && s.variable < 16 && (s.fixed >= 0 || s.variable >= 0) && s.fixed != 0 && s.variable != 0;
            check(sane && !isKnown("heron::melee_attack::on_update::system") && !isKnown("heron::hit_stop::update_timescales::system") && isKnown("heron::jump::update_raising::system") && !isKnown("heron::jump::update_raising::syste"),
                  "59 systems; not the melee update (the mod's own hook hands it the step) and not the hit stop system");
            check(kClockCount == 60 && kMinClocks == 45, "they take 60 clocks; with fewer than 45 of them in the game's list a bullet time is given up");

            // The walk over a list made of every system name of this build, cut as the game cuts them (from behind
            // "getName<void __cdecl " to the first "("), in the order they stand in the exe. Three engine systems
            // have names of over 2,000 characters; the walk has to get past them.
            {
                const char all[] = "ecs::system::getName<void __cdecl ";
                const size_t al = sizeof all - 1;
                struct Name { const char* at; size_t length; };
                std::vector<Name> list;
                size_t longest = 0; int longOnes = 0;
                for (size_t i = 0; i + al < size; i++) {
                    if (g_img[i] != 'e' || memcmp(g_img + i, all, al) != 0) continue;
                    const char* at = (const char*)g_img + i + al; size_t n = 0;
                    while (at[n] && at[n] != '(') n++;
                    list.push_back(Name{at, n});
                    if (n > longest) longest = n;
                    if (n > kMaxName) longOnes++;
                }
                const int n = (int)list.size();
                std::vector<uint8_t> objects((size_t)(n + 1) * kSysSize, 0);
                std::vector<uint8_t*> lists((size_t)n * 16, nullptr);
                static uint8_t fixedClock[kFixedSize], variableClock[kVariableSize];
                int start = -1, firstLong = -1, firstKnown = -1;
                const char* const hitStop = "heron::hit_stop::update_timescales::system";
                for (int i = 0; i < n; i++) {
                    uint8_t* s = objects.data() + (size_t)i * kSysSize; uint16_t id = (uint16_t)i; uint64_t len = list[i].length; uint8_t** e = lists.data() + (size_t)i * 16;
                    memcpy(s + kSysId, &id, 2); memcpy(s + kSysName, &list[i].at, 8); memcpy(s + kSysNameLength, &len, 8); memcpy(s + kSysEnvs, &e, 8);
                    const SysTime* k = len <= kMaxName ? known(list[i].at, list[i].length) : nullptr;
                    if (k) { if (k->fixed >= 0) e[k->fixed] = fixedClock; if (k->variable >= 0) e[k->variable] = variableClock; if (firstKnown < 0) firstKnown = i; }
                    else e[1] = fixedClock;
                    if (len == strlen(hitStop) && memcmp(list[i].at, hitStop, (size_t)len) == 0) start = i;
                    if (len > kMaxName && firstLong < 0) firstLong = i;
                }
                { uint16_t id = 0xffff; memcpy(objects.data() + (size_t)n * kSysSize + kSysId, &id, 2); }
                printf("      %d systems in this build; longest name %d characters, %d names over %d; the first of those is system %d, the first of the table %d, the hit stop system %d\n",
                       n, (int)longest, longOnes, (int)kMaxName, firstLong, firstKnown, start);
                check(n > 2000 && n < kMaxSystems && start >= 0, "every system of this build (over 2,000, fewer than the mod walks at most), the hit stop system among them");
                check(longOnes >= 1, "the build has system names longer than any name of the table (what stopped the walk of 0.11)");
                Clocks c; Slots slots;
                Swap w = engage(objects.data() + (size_t)(start < 0 ? 0 : start) * kSysSize, fixedClock, variableClock, c, slots);
                check(w.systems == n && w.unnamed == longOnes && w.end == 1, "the walk from the hit stop system covers the whole list, the long names passed over");
                check(w.named == kSystemCount && w.swapped == kClockCount && w.other == 0 && w.shared == 0, "all 59 systems of the table are found and their 60 clocks handed over");
                bool others = true;
                for (int i = 0; i < n; i++) { const SysTime* k = list[i].length <= kMaxName ? known(list[i].at, list[i].length) : nullptr; if (!k && lists[(size_t)i * 16 + 1] != fixedClock) others = false; }
                check(others, "no other system of the build loses the game's clock");
                check(release(slots, c) == kClockCount, "and the way back");
            }
        }
    }

    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
