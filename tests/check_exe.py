# Replays the mod's code search (installHook() in witchtime.cpp, patterns.h) over the game's exe on disk:
# every pattern must match exactly once, and the places the mod looks for inside and around the functions
# must be there. Run it after a game update to see whether the mod still finds everything; tests\out\codecheck.exe
# then runs the game's own functions on test data.
# usage: python check_exe.py [path to CONTROLResonant.exe]
import os, re, struct, sys
exe = sys.argv[1] if len(sys.argv) > 1 else 'M:/CONTROL Resonant/CONTROLResonant.exe'
d = open(exe, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
print('timestamp %08X' % struct.unpack_from('<I', d, pe + 8)[0])
nsec = struct.unpack_from('<H', d, pe + 6)[0]; opt = struct.unpack_from('<H', d, pe + 20)[0]
secs = [struct.unpack_from('<8sIIII', d, pe + 24 + opt + 40 * i) for i in range(nsec)]
name, vsz, rva, rsz, raw = [s for s in secs if s[0].startswith(b'.text')][0]
text = d[raw:raw + min(vsz, rsz)]
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'patterns.h'), encoding='utf-8').read()
def pat(k):
    m = re.search(r'const char %s\[\] =\s*((?:\s*"[^"]*")+);' % k, src)
    p = ' '.join(re.findall(r'"([^"]*)"', m.group(1))).split()
    return b''.join(b'.' if t.startswith('?') else re.escape(bytes([int(t, 16)])) for t in p)
def const(k): return int(re.search(r'const unsigned %s = (0x[0-9a-fA-F]+);' % k, src).group(1), 16)
def blob(k): return bytes(int(x, 16) for x in re.search(r'%s\[\] = \{([^}]*)\}' % k, src).group(1).replace(' ', '').split(','))
def target(o): return o + 5 + struct.unpack_from('<i', text, o + 1)[0]
ok = True

# the player's time step (findSteps): three functions, each found by a pattern that starts some bytes into it
fn = {}
for k, at, what in (('kMelee', 'kMeleeAt', 'melee update'), ('kStepOne', 'kStepOneAt', 'time step update, one archetype'), ('kStepAll', 'kStepAllAt', 'time step update, all archetypes')):
    hits = [h.start() for h in re.finditer(pat(k), text, re.S)]
    ok &= len(hits) == 1
    fn[k] = hits[0] - const(at) if len(hits) == 1 else None
    print('%-9s %d match(es) %s  (%s)' % (k, len(hits), ' '.join('fn rva 0x%x' % (rva + h - const(at)) for h in hits[:3]), what))
if ok:
    m = fn['kMelee']; lookups = len(re.findall(pat('kEntityLookup'), text[m:m + const('kMeleeSpan')], re.S))
    one = fn['kStepOne']; store = blob('kStepOneStore'); store_ok = text[one + const('kStepOneStoreAt'):one + const('kStepOneStoreAt') + len(store)] == store
    al = fn['kStepAll']; start = blob('kStepAllStart'); start_ok = text[al + const('kStepAllStartAt'):al + const('kStepAllStartAt') + len(start)] == start
    print('entity lookup inside the melee update: %d   store in the one-archetype form: %s   start of the all-archetypes form: %s' % (lookups, 'ok' if store_ok else 'DIFFERENT', 'ok' if start_ok else 'DIFFERENT'))
    ok &= lookups >= 1 and store_ok and start_ok
# the engine's time step update: the all-archetypes form twice (fixed, variable); for each the one-archetype
# form is the function of that shape that calls the same per-entity body
alls = [h.start() - const('kLocalAllAt') for h in re.finditer(pat('kLocalAll'), text, re.S)]
ones = [h.start() - const('kLocalOneAt') for h in re.finditer(pat('kLocalOne'), text, re.S)]
print('kLocalAll %d match(es) %s  (engine time step update, all archetypes; 2 expected)' % (len(alls), ' '.join('fn rva 0x%x' % (rva + a) for a in alls[:4])))
eng = len(alls) == 2
if eng:
    start = blob('kLocalAllStart')
    for a in alls:
        ok_a = text[a + const('kLocalAllStartAt'):a + const('kLocalAllStartAt') + len(start)] == start and text[a + const('kLocalAllBodyCall')] == 0xE8
        mine = [g for g in ones if text[g + const('kLocalOneBodyCall')] == 0xE8 and target(g + const('kLocalOneBodyCall')) == target(a + const('kLocalAllBodyCall'))] if ok_a else []
        print('   fn rva 0x%x: layout %s, one-archetype form: %s  (of %d functions of that shape)' % (rva + a, 'ok' if ok_a else 'DIFFERENT', ' '.join('rva 0x%x' % (rva + g) for g in mine) or 'NOT FOUND', len(ones)))
        eng &= ok_a and len(mine) == 1
ok &= eng

# the witch time (findBullet): the request for a timescale curve, the curves' update with three places inside
# it, four places elsewhere in the game's code once each, and the engine's clock tick
wt = True; at = {}
for k, where, what in (('kStartCurve', 'kStartCurveAt', 'timescale curve request'), ('kTimescales', 'kTimescalesAt', 'timescale curves update'), ('kClockTick', 'kClockTickAt', 'engine clock tick')):
    hits = [h.start() for h in re.finditer(pat(k), text, re.S)]
    at[k] = hits[0] - const(where) if len(hits) == 1 else None
    if k != 'kClockTick': wt &= len(hits) == 1
    print('%-12s %d match(es) %s  (%s)' % (k, len(hits), ' '.join('fn rva 0x%x' % (rva + h - const(where)) for h in hits[:3]), what))
if at['kTimescales'] is not None:
    f = at['kTimescales']
    inside = {k: len(re.findall(pat(k), text[f:f + const('kTimescalesSpan')], re.S)) for k in ('kTimescalesLoads', 'kTimescalesStore', 'kTimescalesOut')}
    print('inside the curves update: ' + ', '.join('%s %d' % (k, n) for k, n in inside.items()))
    wt &= all(n == 1 for n in inside.values())
for k, what in (('kSchedulerCall', "the scheduler's call of a system"), ('kSystemName', "a system's name"), ('kWorldEnvs', "the world's table of environments"), ('kClockUpdate', "the engine's clock update")):
    n = len(re.findall(pat(k), text, re.S))
    print('%-15s %d match(es)  (%s; 1 expected)' % (k, n, what))
    wt &= n == 1
ok &= wt
print('the mod finds everything' if ok else 'THE MOD WOULD STAY INACTIVE on this build' + ('' if at['kClockTick'] is not None else ' (and the clock tick, which it works without, is missing too)'))
sys.exit(0 if ok else 1)
