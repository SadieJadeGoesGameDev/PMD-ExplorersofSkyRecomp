"""Build the Sadie Edition patches as RAM patch tables for the recomp.

Each patch group is assembled on its own against the vanilla binaries (armips). Code that SkyTemple's patches
put in overlay 36 (ExtraSpace) and our own helpers are relocated into unused ITCM instead, so no extra overlay is
needed. Output:
  runtime/sadie_patches.h  - byte runs per group (vanilla + patched), applied/reverted live by the runtime
  tools/sadie_funcs.json   - functions touched by each group (the lifter makes them fall back to the interpreter
                             while their group is on, so the patched bytes in RAM are what runs)
"""
import json, os, re, shutil, subprocess, sys, tempfile
ROOT = os.environ.get('SKYRECOMP', os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ndsrom import Rom, u32

ARMIPS = os.environ.get('ARMIPS', '/tmp/armips/build/armips')
ST = os.environ.get('SKYTEMPLE_FILES', '/tmp/skytemple-files') + '/skytemple_files/_resources/patches/asm_patches'
ROM = sys.argv[1]
R = ROOT

ITCM_TC = 0x01FFD000     # team control code/data (0x470 bytes)
ITCM_OWN = 0x01FFD500    # our own helpers (defeat penalty, IQ per level)
ITCM_SIZE = 0x400        # size of the fake ITCM file we assemble into, based at 0x01FFD000

rom = Rom(ROM)
vanilla = {'arm9': bytes(rom.arm9 + rom.arm9post), 'itcm': bytes(ITCM_SIZE * 4)}
load = {'arm9': 0x2000000, 'itcm': ITCM_TC}
for k in range(36):
    vanilla['overlay_%04d' % k] = bytes(rom.overlay(k))
    load['overlay_%04d' % k] = u32(rom.y9, k * 32 + 4)


def assemble(main_asm, copy_tree=True, replace=None, params=None, extra_files=()):
    with tempfile.TemporaryDirectory() as t:
        if copy_tree:
            shutil.copytree(ST, t, dirs_exist_ok=True)
        for name, data in vanilla.items():
            open(os.path.join(t, name + '.bin'), 'wb').write(data)
        # the fake ExtraSpace overlay: armips writes team control at file offset 0x10 -> ITCM_TC
        open(os.path.join(t, 'overlay_0036.bin'), 'wb').write(bytes(ITCM_SIZE * 4))
        for fn, data in extra_files:
            open(os.path.join(t, fn), 'w').write(data)
        if replace:
            for fn, rx, rep in replace:
                p = os.path.join(t, fn)
                s = open(p, encoding='utf-8').read()
                open(p, 'w', encoding='utf-8').write(re.sub(rx, rep, s))
        open(os.path.join(t, '__main.asm'), 'w').write(main_asm)
        cmd = [ARMIPS, '__main.asm']
        for k, v in (params or {}).items():
            cmd += ['-equ', k, str(v)]
        r = subprocess.run(cmd, cwd=t, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(r.stdout + r.stderr)
        out = {}
        for name in vanilla:
            out[name] = open(os.path.join(t, name + '.bin'), 'rb').read()
        ov36 = open(os.path.join(t, 'overlay_0036.bin'), 'rb').read()
        if any(ov36):
            # ov36 offset 0x10 == ITCM_TC: shift into our itcm image
            img = bytearray(vanilla['itcm'])
            img[0:len(ov36) - 0x10] = ov36[0x10:]
            out['itcm'] = bytes(img)
        return out


STUB = open(os.path.join(ST, 'header_stub_na.asm')).read()


def opened(binname, includes):
    s = '.open "%s.bin", 0x%x\n' % (binname, load[binname])
    for inc in includes:
        s += '.include "%s"\n' % inc
    return s + '.close\n'


groups = {}

# 0: pick any starter (quiz result -> choose screen); overlay 13 grows into its bss tail
# its new code normally goes past the end of overlay 13 (RAM that belongs to something else in vanilla): move it to ITCM
groups[0] = dict(name='any_starter', out=assemble(
    STUB + '\n' + opened('overlay_0013', ['anonymous_asm_mods/choose_starter/selector_overlay13.asm']),
    replace=[('anonymous_asm_mods/choose_starter/common/patch.asm', r'\.org OverlayStart\+OrgSize',
              '.close\n.open "../../../itcm.bin", 0x%X\n.org 0x%X' % (ITCM_TC, ITCM_TC + 0x600)),
             ('anonymous_asm_mods/choose_starter/common/patch.asm', r'\.fill OverlayStart\+OrgSize\+ExtendSize-\., 0x0', '')]),
    strings={2613: "Then, who would you like to be?"})

# 1: full team control (cipnit), ExtraSpace part relocated to ITCM
groups[1] = dict(name='team_control', out=assemble(
    STUB + '\n.include "cipnit_asm_mods/completeteamcontrol.asm"\n',
    replace=[('cipnit_asm_mods/common/regionSelect.asm', r'_REGION equ "(\w+)"', '_REGION equ "US"'),
             ('cipnit_asm_mods/common/offsetsUS.asm', r'ov_36 equ 023A7080h', 'ov_36 equ 0%Xh' % (ITCM_TC - 0x10))]),
    strings={296: "[CS:S]Control mode set to[CR] [CS:C]automatic[CR][CS:S].[CR]",
             297: "[CS:S]Control mode set to[CR] [CS:E]manual[CR][CS:S].[CR]"})

OWN_HEAD = '.nds\n.arm\n'
# 2: gentler hunger
groups[2] = dict(name='gentle_hunger', out=assemble(OWN_HEAD + '''
.open "overlay_0029.bin", 0x022DC240
.org 0x02310A70
    .word 0x0CCD                ; BELLY_LOST_PER_TURN (was 0x199A): half the drain
.close
''', copy_tree=False))

# 3: keep items on defeat, lose only 10% of money
groups[3] = dict(name='keep_items', out=assemble(OWN_HEAD + '''
.open "itcm.bin", 0x%X
.org 0x%X
LoseTenPercent:                 ; r0 = money carried -> r0 = money - floor(money/10)
    ldr   r1, =0xCCCCCCCD
    umull r2, r3, r0, r1
    sub   r0, r0, r3, lsr #3
    bx    lr
    .pool
.close
.open "overlay_0010.bin", 0x022BCA80
.org 0x022C2C0C
    b     0x022C2C34            ; never randomly discard losable items
.org 0x022C2C48
    bl    LoseTenPercent        ; was: money / 2
    nop
.close
''' % (ITCM_TC, ITCM_OWN), copy_tree=False))

# 4: +5 IQ (a favourite gummi) for every level gained
groups[4] = dict(name='iq_per_level', out=assemble(OWN_HEAD + '''
BoostIQ equ 0x022FA144
.open "itcm.bin", 0x%X
.org 0x%X
IqPerLevel:
    push  {lr}
    mov   r0, r10
    mov   r1, #5
    cmp   r9, #0
    moveq r2, #1
    movne r2, #0
    bl    BoostIQ
    add   r5, r5, #1
    pop   {pc}
.close
.open "overlay_0029.bin", 0x022DC240
.org 0x02303258
    bl    IqPerLevel            ; was: add r5, r5, #1
.close
''' % (ITCM_TC, ITCM_OWN + 0x40), copy_tree=False))

# 5: full team EXP share (100%)
groups[5] = dict(name='team_exp', out=assemble(
    STUB + '\n' + opened('arm9', ['anonymous_asm_mods/exp_share/selector_arm9.asm']) +
    opened('overlay_0019', ['anonymous_asm_mods/exp_share/selector_overlay19.asm']) +
    opened('overlay_0029', ['anonymous_asm_mods/exp_share/selector_overlay29.asm']),
    params={'PercentageShared': 100}))

# 6: anyone can recruit: a defeated enemy may ask to join when any team member lands the final blow, from any distance
#    (vanilla: only the leader, and only from an adjacent tile). The line-of-sight check stays.
groups[6] = dict(name='recruit_anyone', out=assemble(OWN_HEAD + """
.open "overlay_0029.bin", 0x022DC240
.org 0x0230A818
    .word 0x05D00006            ; ldrbeq r0, [r0, #6]  ApplyDamage: was is_team_leader (#7) -> is_not_team_member
    .word 0x03500001            ; cmpeq r0, #1         skip the recruit check only when an enemy landed the blow
.org 0x0230DD40
    .word 0xE1A00000            ; nop  RecruitCheck: was bgt (|dx| > 1 -> no)
.org 0x0230DD58
    .word 0xEA000001            ; b 0x0230DD64  was ble (|dy| > 1 -> no)
.close
""", copy_tree=False))

# ---- diff -> byte runs
inv = json.load(open(R + '/inventory.json'))
def func_of(binname, addr):
    b = 'arm9' if binname == 'arm9' else 'itcm' if binname == 'itcm' else 'overlay%02d' % int(binname[8:])
    if b not in inv:
        return None
    best = None
    for a, f in inv[b]['functions'].items():
        a = int(a, 16)
        if a <= addr and (best is None or a > best):
            best = a
    return (b, best) if best is not None else None

BIN_ID = lambda n: -1 if n == 'arm9' else -2 if n == 'itcm' else int(n[8:])
runs, funcs = [], {}
for g, G in groups.items():
    fs = set()
    for name, new in G['out'].items():
        old = vanilla[name]
        if len(new) > len(old):
            old = old + bytes(len(new) - len(old))
        i = 0
        while i < len(new):
            if new[i] != old[i]:
                j = i
                while j < len(new) and (new[j] != old[j] or (j + 8 < len(new) and new[j:j + 8] != old[j:j + 8])):
                    j += 1
                a = load[name] + i
                runs.append((g, BIN_ID(name), a, old[i:j], new[i:j]))
                if name != 'itcm':
                    for x in range(a & ~3, a + (j - i), 4):
                        f = func_of(name, x)
                        if f: fs.add(f)
                i = j
            else:
                i += 1
    funcs[g] = sorted(fs)
    print(G['name'], 'runs', sum(1 for r in runs if r[0] == g), 'bytes', sum(len(r[3]) for r in runs if r[0] == g),
          'functions', ['%s:%X' % f for f in funcs[g]])

with open(R + '/runtime/sadie_patches.h', 'w') as f:
    f.write('/* generated by tools/sadie_patches.py */\n#include <stdint.h>\n')
    f.write('typedef struct { int group, bin; uint32_t addr, len; const uint8_t *vanilla, *patched; } SadiePatch;\n')
    for i, (g, b, a, old, new) in enumerate(runs):
        f.write('static const uint8_t sp_v%d[] = {%s};\n' % (i, ','.join(str(x) for x in old)))
        f.write('static const uint8_t sp_p%d[] = {%s};\n' % (i, ','.join(str(x) for x in new)))
    f.write('static const SadiePatch SADIE_PATCHES[] = {\n')
    for i, (g, b, a, old, new) in enumerate(runs):
        f.write('  {%d, %d, 0x%Xu, %d, sp_v%d, sp_p%d},\n' % (g, b, a, len(old), i, i))
    f.write('};\n#define SADIE_PATCH_COUNT %d\n' % len(runs))
    f.write('typedef struct { int group, id; const char *text; } SadieString;\n')
    f.write('static const SadieString SADIE_STRINGS[] = {\n')
    n = 0
    for g, G in groups.items():
        for sid, text in G.get('strings', {}).items():
            f.write('  {%d, %d, "%s"},\n' % (g, sid, text.replace('"', '\\"'))); n += 1
    f.write('};\n#define SADIE_STRING_COUNT %d\n' % n)
json.dump({'funcs': {str(g): ['%s:%X' % x for x in fs] for g, fs in funcs.items()},
           'runs': [[g, b, a, len(old)] for g, b, a, old, new in runs]}, open(R + '/tools/sadie_funcs.json', 'w'), indent=1)
print('wrote', len(runs), 'runs')
