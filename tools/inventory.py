"""Build a function inventory for PMD:EoS (US) from pmdsky-debug + call-graph scan."""
import sys, struct, json, glob, os, yaml
ROOT = os.environ.get('SKYRECOMP', os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ndsrom import Rom, u32
SYM = os.environ.get('PMDSKY_DEBUG', '/tmp/pmdsky-debug') + '/symbols'
rom = Rom(sys.argv[1])

def bins():
    out = {'arm9': (0x2000000, bytes(rom.arm9))}
    for k in range(len(rom.y9)//32):
        out['overlay%02d' % k] = (u32(rom.y9, k*32+4), bytes(rom.overlay(k)))
    return out
B = bins()

# ITCM/DTCM autoload: module params via nitrocode footer
mp = u32(rom.arm9post, 4)
auto_start, auto_end, auto_data = struct.unpack_from('<III', rom.arm9, mp)
print('module params @%x autoload list %x-%x data %x' % (mp, auto_start, auto_end, auto_data))
a = auto_start - 0x2000000; src = auto_data - 0x2000000
for i in range((auto_end - auto_start)//12):
    dst, size, bss = struct.unpack_from('<III', rom.arm9, a + i*12)
    print('  autoload -> %x size %x bss %x (from arm9+%x)' % (dst, size, bss, src))
    if dst < 0x2000000 and size:
        B['itcm'] = (dst, bytes(rom.arm9[src:src+size]))
    src += size

def load(fn, key, ver='NA'):
    y = yaml.safe_load(open(fn))
    blk = next(iter(y.values()))
    res = []
    for f in blk.get('functions', []):
        ad = f['address'].get(ver)
        if ad is None: continue
        for x in (ad if isinstance(ad, list) else [ad]):
            res.append((x, f['name']))
    return res

named = {}
named['arm9'] = load(f'{SYM}/arm9.yml','arm9') + load(f'{SYM}/arm9/libs.yml','arm9')
named['itcm'] = load(f'{SYM}/arm9/itcm.yml','itcm','NA-ITCM')
for k in range(36):
    p = f'{SYM}/overlay{k:02d}.yml'
    named['overlay%02d' % k] = load(p, k) if os.path.exists(p) else []
named['overlay29'] += load(f'{SYM}/overlay29/move_effects.yml', 29)

def in_bin(name, addr):
    base, data = B[name]; return base <= addr < base + len(data)

total = {'named':0,'found':0,'thumb':0}
inv = {}
for name, (base, data) in B.items():
    funcs = {a: n for a, n in named.get(name, []) if in_bin(name, a & ~1)}
    thumb = {a & ~1 for a in funcs if a & 1}
    funcs = {a & ~1: n for a, n in funcs.items()}
    # call-graph scan: ARM BL/BLX imm targets inside this binary (and arm9 for overlays)
    found = set()
    for o in range(0, len(data)-3, 4):
        w = u32(data, o)
        if (w >> 25) & 7 == 5:
            off = w & 0xFFFFFF
            if off & 0x800000: off -= 1 << 24
            t = base + o + 8 + off*4
            if (w >> 28) == 0xF:   # BLX imm -> thumb
                t += ((w >> 24) & 1) * 2
                if in_bin(name, t): thumb.add(t); found.add(t)
            elif (w >> 24) & 1 and in_bin(name, t):
                found.add(t)
    # function-pointer scan: aligned words pointing at a prologue in this binary
    for nm2, (b2, d2) in B.items():
        if nm2 != name and not (name in ('arm9', 'itcm') or nm2 in ('arm9', 'itcm')):
            continue
        for o in range(0, len(d2) - 3, 4):
            v = u32(d2, o)
            t = v & ~1
            if not in_bin(name, t): continue
            if v & 1:
                hw = struct.unpack_from('<H', data, t - base)[0]
                if hw & 0xFF00 == 0xB500: thumb.add(t); found.add(t)
            elif t % 4 == 0 and u32(data, t - base) & 0xFFFF4000 == 0xE92D4000:
                found.add(t)
    allf = set(funcs) | found
    inv[name] = {'base': base, 'size': len(data),
                 'functions': {hex(a): {'name': funcs.get(a, 'sub_%X' % a), 'thumb': a in thumb} for a in sorted(allf)}}
    total['named'] += len(funcs); total['found'] += len(allf); total['thumb'] += len(thumb & allf)
    print('%-10s base %08x size %6x  named %5d  total %5d  thumb %4d' % (name, base, len(data), len(funcs), len(allf), len(thumb & allf)))
print(total)
json.dump(inv, open(os.path.join(ROOT, 'inventory.json'), 'w'), indent=1)
