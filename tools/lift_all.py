"""Lift every ARM function in the inventory to C; report coverage; write per-binary .c files."""
import json, os, sys, collections
ROOT = os.environ.get('SKYRECOMP', os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lifter import Func, Lifter, Unsupported
from thumb import ThumbFunc, ThumbLifter
NATIVE = {('arm9', 0x208FEA4), ('arm9', 0x20900B0), ('arm9', 0x20900B8), ('arm9', 0x2079D08),
          ('arm9', 0x208FC7C), ('arm9', 0x208FC8C), ('arm9', 0x208FE3C), ('arm9', 0x208FE48), ('arm9', 0x208FE84),
          ('overlay29', 0x2336F4C), ('overlay29', 0x233711C), ('overlay29', 0x23372A4),
          ('arm9', 0x201B9B0),   # sprite X cull (widescreen)
          ('overlay11', 0x22EEAAC), ('overlay11', 0x22EEBF8), ('overlay11', 0x22EED98), ('overlay11', 0x22EEFC0),  # overworld BG window refresh
          ('overlay11', 0x22F1F3C),   # ground camera limits (widescreen)
          ('arm9', 0x2025788), ('arm9', 0x202A5CC), ('overlay11', 0x2300F50), ('overlay31', 0x2382B54),   # Recomp Options menu
          ('arm9', 0x20040AC)}   # LoadOverlay: Sadie Edition patches
EXCLUDE = {('arm9', 0x2079D54), ('itcm', 0x1FF95E8), ('itcm', 0x1FF9650)}          # OS_LoadContext: always interpreted (exception return)
RENAME = {('arm9', 0x2079D08): 'f_arm9_2079D08_orig',
          ('overlay29', 0x2336F4C): 'f_overlay29_2336F4C_orig',   # dungeon BG: full redraw  (widescreen: draw 45 columns)
          ('overlay29', 0x233711C): 'f_overlay29_233711C_orig',   # dungeon BG: edge column
          ('overlay29', 0x23372A4): 'f_overlay29_23372A4_orig',
          ('overlay11', 0x22F1F3C): 'f_overlay11_22F1F3C_orig',
          ('arm9', 0x2025788): 'f_arm9_2025788_orig', ('arm9', 0x202A5CC): 'f_arm9_202A5CC_orig',
          ('overlay11', 0x2300F50): 'f_overlay11_2300F50_orig', ('overlay31', 0x2382B54): 'f_overlay31_2382B54_orig',
          ('arm9', 0x20040AC): 'f_arm9_20040AC_orig'}   # dungeon BG: edge row   # OS_SaveContext: wrapped natively
from ndsrom import Rom, u32
# instruction patches applied only to the code we lift (fingerprints still use the original bytes)
CODE_PATCH = {('overlay29', 0x22E2CD0): (0xE3500006, 0xE3500008),   # IsPositionVisibleAndOnScreen: 6 -> 8 tiles in x (widescreen)
              ('overlay29', 0x22E2D6C): (0xE3500006, 0xE3500008)}   # sibling check, same

inv = json.load(open(os.path.join(ROOT, 'inventory.json')))
# Sadie Edition: functions touched by each patch group run in the interpreter (on the patched RAM) while the group is on
SADIE = json.load(open(os.path.join(ROOT, 'tools', 'sadie_funcs.json')))
SADIE_FUNC = {}
for g, fl in SADIE['funcs'].items():
    for x in fl:
        bname, a = x.split(':'); SADIE_FUNC.setdefault((bname, int(a, 16)), []).append(int(g))
SADIE_WORDS = set()
for g, binid, a, n in SADIE['runs']:
    bname = 'arm9' if binid == -1 else 'itcm' if binid == -2 else 'overlay%02d' % binid
    for x in range(a & ~3, a + n, 4): SADIE_WORDS.add((bname, x))
rom = Rom(sys.argv[1])
outdir = os.path.join(ROOT, 'gen')
os.makedirs(outdir, exist_ok=True)

data = {'arm9': bytes(rom.arm9)}
for k in range(36):
    data['overlay%02d' % k] = bytes(rom.overlay(k))
mp = u32(rom.arm9post, 4)
data['itcm'] = bytes(rom.arm9[0xB3380:0xB3380 + 0x4000])
lift_data = dict(data)
for (b, a), (old, new) in CODE_PATCH.items():
    o = a - (0x2000000 if b == 'arm9' else u32(rom.y9, int(b[7:]) * 32 + 4))
    d = bytearray(lift_data[b]); assert u32(d, o) == old, hex(a); d[o:o+4] = new.to_bytes(4, 'little'); lift_data[b] = bytes(d)

starts = {b: {int(a, 16) for a in v['functions']} for b, v in inv.items()}
thumbs = {b: {int(a, 16) for a, f in v['functions'].items() if f['thumb']} for b, v in inv.items()}
ranges = {b: (v['base'], v['base'] + v['size']) for b, v in inv.items()}

def cname(b, a):
    return 'f_%s_%X' % (b, a & ~1)

def name_of(t, f):
    if ranges[f.bin][0] <= t < ranges[f.bin][1]:
        return cname(f.bin, t)
    for b in ('arm9', 'itcm'):
        if ranges[b][0] <= t < ranges[b][1]:
            return cname(b, t)
    return 'CALL_ADDR(0x%Xu)' % t   # into an overlay region: dispatch at runtime

# pre-pass: BL/BLX from any binary into arm9/itcm adds function starts there
for b0, d0 in data.items():
    base0 = ranges[b0][0]
    for o in range(0, len(d0) - 3, 4):
        w = u32(d0, o)
        if (w >> 25) & 7 == 5 and ((w >> 24) & 1 or (w >> 28) == 0xF):
            off = w & 0xFFFFFF
            if off & 0x800000: off -= 1 << 24
            t = base0 + o + 8 + off * 4
            if (w >> 28) == 0xF: t += ((w >> 24) & 1) * 2
            for tb in ('arm9', 'itcm'):
                if tb != b0 and ranges[tb][0] <= t < ranges[tb][1] and t not in starts[tb]:
                    starts[tb].add(t & ~1)
                    if (w >> 28) == 0xF: thumbs[tb].add(t & ~1)

stats = collections.Counter()
emitted = []
unsup = collections.Counter()
fails = []
extra_calls = collections.defaultdict(set)
for b in inv:
    base, end = ranges[b]
    lines = ['#include "cpu.h"', '#include "settings.h"', 'void recomp_interp_call(uint32_t);', '#define CALL_ADDR(a) call_addr(a)']
    bodies = []
    decls = set()
    todo = sorted(starts[b])
    seen = set()
    while todo:
        a = todo.pop(0)
        if a in seen: continue
        seen.add(a)
        if (b, a) in EXCLUDE:
            stats['excluded'] += 1; continue
        if (b, a) in NATIVE and (b, a) not in RENAME:
            stats['native'] += 1; continue
        best = None
        for is_thumb in ((a in thumbs[b]), not (a in thumbs[b])):
            try:
                f = (ThumbFunc if is_thumb else Func)(b, base, lift_data[b], a, starts[b])
            except Unsupported as e:
                continue
            L = (ThumbLifter if is_thumb else Lifter)(f, name_of)
            body = L.lift(RENAME.get((b, a), cname(b, a)))
            if (b, a) in SADIE_FUNC:
                cond = ' || '.join('recomp_settings.sadie[%d]' % g for g in SADIE_FUNC[(b, a)])
                k = body.index('{') + 1
                body = body[:k] + '\n    if (%s) { recomp_interp_call(0x%Xu); return; }   /* Sadie Edition patch active */' % (cond, a) + body[k:]
            if best is None:
                best = (is_thumb, f, L, body)
            elif not L.unsupported:
                best = (is_thumb, f, L, body)   # other mode decodes cleanly
            if not L.unsupported:
                break
        if best is None:
            fails.append((b, a, 'discover')); stats['discover_fail'] += 1; continue
        is_thumb, f, L, body = best
        if is_thumb: thumbs[b].add(a)
        else: thumbs[b].discard(a)
        if L.unsupported and hex(a) not in inv[b]['functions'] or (L.unsupported and inv[b]['functions'][hex(a)]['name'].startswith('sub_')):
            stats['rejected_garbage'] += 1; continue
        for t in f.calls | f.tailcalls:
            if is_thumb and t in f.tailcalls: t |= 1
            ta = t & ~1
            if base <= ta < end and ta not in starts[b]:
                starts[b].add(ta); todo.append(ta); stats['new_from_calls'] += 1
                if t & 1: thumbs[b].add(ta)
        if is_thumb: stats['thumb_functions'] += 1
        bodies.append(body)
        if (b, a) not in RENAME: emitted.append((b, a, is_thumb))
        for u in L.unsupported:
            unsup[u[2]] += 1
        stats['functions'] += 1
        stats['insns'] += len(f.insns)
        stats['unsupported'] += len(L.unsupported)
    # prototypes for everything referenced
    protos = set()
    for body in bodies:
        for tok in body.split():
            if tok.startswith('f_') and '(' in tok:
                protos.add(tok.split('(')[0])
    lines += ['void %s(void);' % p for p in sorted(protos)]
    _p = os.path.join(outdir, b + '.c'); _t = '\n'.join(lines + bodies) + '\n'
    if not os.path.exists(_p) or open(_p).read() != _t: open(_p, 'w').write(_t)

print(dict(stats))
print('unsupported kinds:', unsup.most_common(10))
print('discover failures (first 10):', fails[:10])


# ---------------- dispatch table ----------------
ovs = [b for b in inv if b.startswith('overlay')]
def word(b, x):
    base = ranges[b][0]; o = x - base
    return u32(data[b], o) if 0 <= o <= len(data[b]) - 4 else None
fp = {}
for b in ovs:
    lo, hi = ranges[b]
    others = [o for o in ovs if o != b and ranges[o][0] < hi and lo < ranges[o][1]]
    cands = sorted(a for (bb, a, t) in emitted if bb == b and (b, a) not in SADIE_WORDS)
    picks = []
    need = set(others)
    step = max(1, len(cands) // 16)
    for a in cands[::step] + cands:
        w = word(b, a)
        diff = {o for o in others if word(o, a) != w}
        if diff & need or (not need and len(picks) < 4):
            picks.append((a, w)); need -= diff
        if not need and len(picks) >= 4:
            break
    if need:
        print('WARNING: overlay %s not distinguishable from %s' % (b, need))
    fp[b] = picks[:8]
L = ['#include "dispatch.h"']
for b, a, t in emitted + [(x[0], x[1], False) for x in NATIVE]:
    L.append('void %s(void);' % cname(b, a))
L.append('const OverlayFP OVERLAY_FP[36] = {')
for k in range(36):
    b = 'overlay%02d' % k
    ws = fp.get(b, [])
    L.append('  { %d, { %s } },' % (len(ws), ', '.join('{0x%Xu, 0x%Xu}' % x for x in ws) or '{0,0}'))
L.append('};')
L.append('const NativeFunc NATIVE_FUNCS[] = {')
for b, a, t in sorted(emitted + [(x[0], x[1], False) for x in NATIVE], key=lambda e: e[1]):
    ov = int(b[7:]) if b.startswith('overlay') else -1
    w0 = word(b, a) if b in data and b != 'itcm' else 0
    sm = sum(1 << g for g in SADIE_FUNC.get((b, a), []))
    L.append('  {0x%Xu, %d, %d, %d, 0x%Xu, %s},' % (a, int(t), ov, sm, (w0 or 0), cname(b, a)))
L.append('};')
L.append('const unsigned NATIVE_FUNC_COUNT = sizeof(NATIVE_FUNCS) / sizeof(NATIVE_FUNCS[0]);')
open(os.path.join(outdir, 'dispatch.c'), 'w').write('\n'.join(L) + '\n')
print('dispatch entries', len(emitted))

# stubs for any function referenced but not emitted -> runtime dispatch / interpreter
import re, glob
defined = {cname(b, a) for b, a, t in emitted} | {cname(x[0], x[1]) for x in NATIVE}
referenced = set()
for fn in glob.glob(os.path.join(outdir, '*.c')):
    if fn.endswith('stubs.c'): continue
    referenced |= set(re.findall(r'\b(f_(?:arm9|itcm|overlay\d\d)_[0-9A-F]+)\(\)', open(fn).read()))
missing = sorted(referenced - defined)
st = ['#include "cpu.h"'] + ['void %s(void) { call_addr(0x%su); }' % (m, m.rsplit('_', 1)[1]) for m in missing]
open(os.path.join(outdir, 'stubs.c'), 'w').write('\n'.join(st) + '\n')
print('stubs', len(missing))
