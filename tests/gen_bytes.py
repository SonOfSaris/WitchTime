# Writes game_bytes.inc for fakegame.cpp: the bytes of the game functions the mod hooks, taken from the
# installed exe by the mod's own patterns (patterns.h), and the places inside and around them the mod looks
# for. The file is the game's own code and is not distributed: make it here from your installed exe.
# usage: python gen_bytes.py [path to CONTROLResonant.exe]
import os, re, struct, sys
here = os.path.dirname(os.path.abspath(__file__))
exe = sys.argv[1] if len(sys.argv) > 1 else 'M:/CONTROL Resonant/CONTROLResonant.exe'
d = open(exe, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]; opt = struct.unpack_from('<H', d, pe + 20)[0]
secs = [struct.unpack_from('<8sIIII', d, pe + 24 + opt + 40 * i) for i in range(nsec)]
name, vsz, rva, rsz, raw = [s for s in secs if s[0].startswith(b'.text')][0]
text = d[raw:raw + min(vsz, rsz)]
src = open(os.path.join(here, '..', 'patterns.h'), encoding='utf-8').read()
def rx(k):
    m = re.search(r'const char %s\[\] =\s*((?:\s*"[^"]*")+);' % k, src)
    p = ' '.join(re.findall(r'"([^"]*)"', m.group(1))).split()
    return b''.join(b'.' if t.startswith('?') else re.escape(bytes([int(t, 16)])) for t in p)
def plen(k):
    m = re.search(r'const char %s\[\] =\s*((?:\s*"[^"]*")+);' % k, src)
    return len(' '.join(re.findall(r'"([^"]*)"', m.group(1))).split())
def pattern(k):
    hits = [h.start() for h in re.finditer(rx(k), text, re.S)]
    assert len(hits) == 1, (k, len(hits))
    return hits[0], plen(k)
def const(k): return int(re.search(r'const unsigned %s = (0x[0-9a-fA-F]+);' % k, src).group(1), 16)
def target(o): return o + 5 + struct.unpack_from('<i', text, o + 1)[0]
def within(k, start, span):
    hits = [h.start() for h in re.finditer(rx(k), text[start:start + span], re.S)]
    assert len(hits) == 1, (k, len(hits))
    return text[start + hits[0]:start + hits[0] + plen(k)], hits[0]
def once(k):
    o, n = pattern(k)
    return text[o:o + n], o
# the melee update: its start (as long as the mod's pattern reaches) and the entity lookup inside it
m, nm = pattern('kMelee')
melee = m - const('kMeleeAt')
melee_start = text[melee:m + nm]
lkm = re.search(rx('kEntityLookup'), text[melee:melee + const('kMeleeSpan')], re.S)
assert lkm, 'entity lookup'
lookup = lkm.group()                                   # (the first of the lookups inside; the mod wants at least one)
# the two forms of hit_stop::update_timescales, whole
o, no = pattern('kStepOne')
one = o - const('kStepOneAt')
step_one = text[one:one + 0x113]
assert step_one[-7:] == bytes([0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5C, 0xC3]) and step_one[0x71] == 0xE8 and step_one[0xdc] == 0xE8, 'update_timescales, one archetype'
a, na = pattern('kStepAll')
al = a - const('kStepAllAt')
step_all = text[al:al + 0x252]
assert step_all[-1] == 0xC3 and text[al + 0x252] == 0xCC and step_all[0x15c] == 0xE8 and step_all[0x1ba] == 0xE8, 'update_timescales, all archetypes'
# the engine's time step update: fixed and variable system, each in an all-archetypes and a one-archetype form
alls = sorted(h.start() - const('kLocalAllAt') for h in re.finditer(rx('kLocalAll'), text, re.S))
assert len(alls) == 2, alls
ones = [h.start() - const('kLocalOneAt') for h in re.finditer(rx('kLocalOne'), text, re.S)]
local = []
for f in alls:
    callee = target(f + const('kLocalAllBodyCall'))
    mine = [g for g in ones if text[g + const('kLocalOneBodyCall')] == 0xE8 and target(g + const('kLocalOneBodyCall')) == callee]
    assert len(mine) == 1, mine
    fa = text[f:f + 0x254]; fo = text[mine[0]:mine[0] + 0xe1]
    assert fa[-1] == 0xC3 and fa[0x144] == 0xE8 and fa[0x16b] == 0xE8 and fa[0x1cc] == 0xE8, 'engine update, all archetypes'
    assert fo[-1] == 0xC3 and fo[0x4e] == 0xE8 and fo[0xac] == 0xE8, 'engine update, one archetype'
    local.append((fa, fo))
# the request for a timescale curve, whole (its one call goes to a stand-in); the start of the curves' update
# (as far as the mod's pattern reaches), the three places inside it and its end; the four places elsewhere in
# the game's code the mod wants to see once each; the engine's clock tick, whole
sc, nsc = pattern('kStartCurve')
start_curve_at = sc - const('kStartCurveAt')
start_curve = text[start_curve_at:start_curve_at + 0x3f]
assert start_curve[:6] == bytes([0x40, 0x53, 0x48, 0x83, 0xEC, 0x40]) and start_curve[0x32] == 0xE8 and start_curve[-1] == 0xC3, 'timescale curve request'
ts, nts = pattern('kTimescales')
timescales_at = ts - const('kTimescalesAt')
timescales_start = text[timescales_at:ts + nts]
assert timescales_start[:5] == bytes([0x48, 0x89, 0x5C, 0x24, 0x10]) and timescales_start[-3:] == bytes([0x48, 0x03, 0xFB]), 'timescale update'
timescales_loads, tl_at = within('kTimescalesLoads', timescales_at, const('kTimescalesSpan'))
timescales_store, tst_at = within('kTimescalesStore', timescales_at, const('kTimescalesSpan'))
timescales_out, to_at = within('kTimescalesOut', timescales_at, const('kTimescalesSpan'))
te = timescales_at + to_at + len(timescales_out)
timescales_end = text[te:te + 0x32]
assert timescales_end[:8] == bytes([0x48, 0x8B, 0x9C, 0x24, 0xA8, 0, 0, 0]) and timescales_end[-8:] == bytes([0x41, 0x5F, 0x41, 0x5E, 0x5F, 0x5E, 0x5D, 0xC3]), 'end of the timescale update'
scheduler_call, sched_at = once('kSchedulerCall')
system_name, sysname_at = once('kSystemName')
world_envs, wenv_at = once('kWorldEnvs')
clock_update, clock_at = once('kClockUpdate')
ct, nct = pattern('kClockTick')
clock_tick_at = ct - const('kClockTickAt')
clock_tick = text[clock_tick_at:ct + nct]
assert clock_tick[:4] == bytes([0x80, 0x79, 0x20, 0x00]) and clock_tick[-1] == 0xC3 and len(clock_tick) == 35, 'clock tick'
def arr(b): return ',\n    '.join(', '.join('0x%02X' % c for c in b[i:i + 20]) for i in range(0, len(b), 20))
out = '// Written by gen_bytes.py from the game exe (timestamp %08X). Not for distribution.\n' % struct.unpack_from('<I', d, pe + 8)[0]
for name, b in (('MELEE_START', melee_start), ('ENTITY_LOOKUP', lookup), ('STEP_ONE', step_one), ('STEP_ALL', step_all),
                ('LOCAL_ALL_FIXED', local[0][0]), ('LOCAL_ONE_FIXED', local[0][1]), ('LOCAL_ALL_VARIABLE', local[1][0]), ('LOCAL_ONE_VARIABLE', local[1][1]),
                ('START_CURVE', start_curve), ('TIMESCALES_START', timescales_start), ('TIMESCALES_LOADS', timescales_loads), ('TIMESCALES_STORE', timescales_store),
                ('TIMESCALES_OUT', timescales_out), ('TIMESCALES_END', timescales_end),
                ('SCHEDULER_CALL', scheduler_call), ('SYSTEM_NAME', system_name), ('WORLD_ENVS', world_envs), ('CLOCK_UPDATE', clock_update), ('CLOCK_TICK', clock_tick)):
    out += 'const unsigned k%sSize = %d;\n#define %s_BYTES \\\n    %s\n' % (name.title().replace('_', ''), len(b), name, arr(b).replace('\n', ' \\\n'))
open(os.path.join(here, 'game_bytes.inc'), 'w', encoding='utf-8', newline='\n').write(out)
print('melee update start %d bytes at rva 0x%x, time step updates at rva 0x%x (%d bytes) and 0x%x (%d bytes), engine updates at rva 0x%x, 0x%x (all) 0x%x, 0x%x (one)' %
      (len(melee_start), rva + melee, rva + one, len(step_one), rva + al, len(step_all), rva + alls[0], rva + alls[1], rva + ones[0] if ones else 0, rva + ones[-1] if ones else 0))
print('timescale curve request at rva 0x%x, timescale update at rva 0x%x (loads +0x%x, store +0x%x, world scale +0x%x); scheduler call at rva 0x%x, system name 0x%x, world environments 0x%x, clock update 0x%x, clock tick 0x%x' %
      (rva + start_curve_at, rva + timescales_at, tl_at, tst_at, to_at, rva + sched_at, rva + sysname_at, rva + wenv_at, rva + clock_at, rva + clock_tick_at))
