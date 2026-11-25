"""Thumb (ARMv5TE) -> C lifter. Produces the same C conventions as lifter.py."""
import struct
from lifter import Unsupported, COND


def u16(b, o):
    return struct.unpack_from('<H', b, o)[0]


class ThumbFunc:
    def __init__(self, binname, base, data, entry, known_starts):
        self.bin, self.base, self.data, self.entry = binname, base, data, entry
        self.known = known_starts
        self.insns = {}     # addr -> (hw, size, hw2)
        self.labels = {entry}
        self.calls = set()
        self.tailcalls = set()
        self.discover()

    def hw(self, a):
        o = a - self.base
        if not (0 <= o <= len(self.data) - 2):
            raise Unsupported('outside binary %x' % a)
        return u16(self.data, o)

    def discover(self):
        todo = [self.entry]
        while todo:
            a = todo.pop()
            while a not in self.insns:
                h = self.hw(a)
                size, h2, stop = 2, None, False
                top5 = h >> 11
                if top5 == 0x1E:                       # BL/BLX prefix
                    h2 = self.hw(a + 2); size = 4
                    off = ((h & 0x7FF) << 12) | ((h2 & 0x7FF) << 1)
                    if off & 0x400000: off -= 1 << 23
                    t = a + 4 + off
                    if h2 >> 11 == 0x1D:
                        self.calls.add(t & ~3)        # BLX -> ARM
                    else:
                        self.calls.add(t | 1)
                elif h >> 12 == 0xD and (h >> 8) & 0xF < 0xE:   # Bcc
                    off = h & 0xFF
                    if off & 0x80: off -= 256
                    t = a + 4 + off * 2
                    self.labels.add(t); todo.append(t)
                elif top5 == 0x1C:                     # B
                    off = h & 0x7FF
                    if off & 0x400: off -= 2048
                    t = a + 4 + off * 2
                    if (t | 1) in self.known or t in self.known:
                        self.tailcalls.add(t)
                    else:
                        self.labels.add(t); todo.append(t)
                    stop = True
                elif h & 0xFF00 == 0x4700:             # BX/BLX reg
                    if not (h >> 7) & 1:
                        stop = True
                elif h & 0xFF00 == 0xBD00:             # POP {.., pc}
                    stop = True
                elif h & 0xFF87 == 0x4687:             # MOV pc, rX
                    stop = True
                self.insns[a] = (h, size, h2)
                if stop:
                    break
                a += size


class ThumbLifter:
    def __init__(self, func, name_of):
        self.f, self.name_of = func, name_of
        self.unsupported = []

    def call(self, t):
        n = self.name_of(t, self.f)
        return n if n.startswith('CALL_ADDR') else n + '()'

    def r(self, n, a):
        return '0x%Xu' % (a + 4) if n == 15 else 'R[%d]' % n

    def body(self, a, h, h2):
        top3, top5 = h >> 13, h >> 11
        rd, rs = h & 7, (h >> 3) & 7
        if top3 == 0 and (h >> 11) & 3 != 3:           # shift imm
            op, amt = (h >> 11) & 3, (h >> 6) & 0x1F
            m = 'R[%d]' % rs
            if op == 0:
                if amt == 0: return ['R[%d] = %s; setNZ(R[%d]);' % (rd, m, rd)]
                return ['{ uint32_t v = %s; C = (v >> %d) & 1; R[%d] = v << %d; setNZ(R[%d]); }' % (m, 32 - amt, rd, amt, rd)]
            if op == 1:
                if amt == 0: return ['{ uint32_t v = %s; C = v >> 31; R[%d] = 0; setNZ(0); }' % (m, rd)]
                return ['{ uint32_t v = %s; C = (v >> %d) & 1; R[%d] = v >> %d; setNZ(R[%d]); }' % (m, amt - 1, rd, amt, rd)]
            if amt == 0: amt = 32
            return ['{ uint32_t v = %s; C = (v >> %d) & 1; R[%d] = (uint32_t)((int32_t)v >> %d); setNZ(R[%d]); }' % (m, min(amt - 1, 31), rd, min(amt, 31), rd)]
        if top5 == 3:                                   # add/sub reg/imm3
            i, sub = (h >> 10) & 1, (h >> 9) & 1
            v = '%du' % ((h >> 6) & 7) if i else 'R[%d]' % ((h >> 6) & 7)
            if sub:
                return ['{ uint32_t a_ = R[%d], b_ = %s, r_ = a_ - b_; setsub(a_, b_, r_); R[%d] = r_; }' % (rs, v, rd)]
            return ['{ uint32_t a_ = R[%d], b_ = %s, r_ = a_ + b_; setadd(a_, b_, r_); R[%d] = r_; }' % (rs, v, rd)]
        if top3 == 1:                                   # mov/cmp/add/sub imm8
            op, d, imm = (h >> 11) & 3, (h >> 8) & 7, h & 0xFF
            if op == 0: return ['R[%d] = %du; setNZ(R[%d]);' % (d, imm, d)]
            if op == 1: return ['{ uint32_t a_ = R[%d], r_ = a_ - %du; setsub(a_, %du, r_); }' % (d, imm, imm)]
            if op == 2: return ['{ uint32_t a_ = R[%d], r_ = a_ + %du; setadd(a_, %du, r_); R[%d] = r_; }' % (d, imm, imm, d)]
            return ['{ uint32_t a_ = R[%d], r_ = a_ - %du; setsub(a_, %du, r_); R[%d] = r_; }' % (d, imm, imm, d)]
        if h >> 10 == 0x10:                             # ALU
            op = (h >> 6) & 0xF
            D, S = 'R[%d]' % rd, 'R[%d]' % rs
            t = {0: '%s & %s', 1: '%s ^ %s', 12: '%s | %s', 14: '%s & ~%s'}
            if op in t: return ['%s = %s; setNZ(%s);' % (D, t[op] % (D, S), D)]
            if op == 2: return ['%s = lsl_reg_c(%s, %s & 0xFF); C = SHC; setNZ(%s);' % (D, D, S, D)]
            if op == 3: return ['%s = lsr_reg_c(%s, %s & 0xFF); C = SHC; setNZ(%s);' % (D, D, S, D)]
            if op == 4: return ['%s = asr_reg_c(%s, %s & 0xFF); C = SHC; setNZ(%s);' % (D, D, S, D)]
            if op == 7: return ['%s = ror_reg_c(%s, %s & 0xFF); C = SHC; setNZ(%s);' % (D, D, S, D)]
            if op == 5: return ['{ uint32_t a_ = %s, b_ = %s, r_ = a_ + b_ + C; setadc(a_, b_, r_); %s = r_; }' % (D, S, D)]
            if op == 6: return ['{ uint32_t a_ = %s, b_ = %s, r_ = a_ - b_ - !C; setsbc(a_, b_, r_); %s = r_; }' % (D, S, D)]
            if op == 8: return ['setNZ(%s & %s);' % (D, S)]
            if op == 9: return ['{ uint32_t b_ = %s, r_ = 0 - b_; setsub(0, b_, r_); %s = r_; }' % (S, D)]
            if op == 10: return ['{ uint32_t a_ = %s, b_ = %s; setsub(a_, b_, a_ - b_); }' % (D, S)]
            if op == 11: return ['{ uint32_t a_ = %s, b_ = %s; setadd(a_, b_, a_ + b_); }' % (D, S)]
            if op == 13: return ['%s = %s * %s; setNZ(%s);' % (D, D, S, D)]
            if op == 15: return ['%s = ~%s; setNZ(%s);' % (D, S, D)]
        if h >> 10 == 0x11:                             # hi reg ops / bx
            op = (h >> 8) & 3
            d = (h & 7) | ((h >> 4) & 8)
            s_ = (h >> 3) & 0xF
            S = self.r(s_, a)
            if op == 0:
                if d == 15: return ['indirect_jump(R[15] = 0x%Xu + %s); return;' % (a + 4, S)]
                return ['R[%d] = %s + %s;' % (d, self.r(d, a), S)]
            if op == 1: return ['{ uint32_t a_ = %s, b_ = %s; setsub(a_, b_, a_ - b_); }' % (self.r(d, a), S)]
            if op == 2:
                if d == 15:
                    return ['RET = R[14]; return;'] if s_ == 14 else ['indirect_jump(%s); return;' % S]
                return ['R[%d] = %s;' % (d, S)]
            if (h >> 7) & 1:
                return ['{ uint32_t t = %s; R[14] = 0x%Xu; call_indirect(t); }' % (S, (a + 2) | 1),
                        'if (RET != 0x%Xu) { divert(entry_lr, entry_sp); return; }' % ((a + 2) | 1)]
            return ['RET = R[14]; return;'] if s_ == 14 else ['indirect_jump(%s); return;' % S]
        if top5 == 9:                                   # ldr pc-rel
            addr = ((a + 4) & ~3) + (h & 0xFF) * 4
            return ['R[%d] = rd32(0x%Xu);' % ((h >> 8) & 7, addr)]
        if h >> 12 == 5:                                # reg offset
            ro = (h >> 6) & 7
            addr = '(R[%d] + R[%d])' % (rs, ro)
            op = (h >> 9) & 7
            return [['wr32(%s, R[%d]);', 'wr16(%s, R[%d]);', 'wr8(%s, R[%d]);', 'R[%d] = rds8(%s);',
                     'R[%d] = rd32_rot(%s);', 'R[%d] = rd16(%s);', 'R[%d] = rd8(%s);', 'R[%d] = rds16(%s);'][op]
                    % ((addr, rd) if op < 3 else (rd, addr))]
        if top3 == 3:                                   # imm offset word/byte
            b, l, off = (h >> 12) & 1, (h >> 11) & 1, (h >> 6) & 0x1F
            off = off if b else off * 4
            addr = '(R[%d] + %du)' % (rs, off)
            if l: return ['R[%d] = %s(%s);' % (rd, 'rd8' if b else 'rd32_rot', addr)]
            return ['%s(%s, R[%d]);' % ('wr8' if b else 'wr32', addr, rd)]
        if h >> 12 == 8:                                # halfword imm
            addr = '(R[%d] + %du)' % (rs, ((h >> 6) & 0x1F) * 2)
            if (h >> 11) & 1: return ['R[%d] = rd16(%s);' % (rd, addr)]
            return ['wr16(%s, R[%d]);' % (addr, rd)]
        if h >> 12 == 9:                                # sp-relative
            d, addr = (h >> 8) & 7, '(R[13] + %du)' % ((h & 0xFF) * 4)
            if (h >> 11) & 1: return ['R[%d] = rd32_rot(%s);' % (d, addr)]
            return ['wr32(%s, R[%d]);' % (addr, d)]
        if h >> 12 == 0xA:                              # add rd, pc/sp, #imm
            d, imm = (h >> 8) & 7, (h & 0xFF) * 4
            if (h >> 11) & 1: return ['R[%d] = R[13] + %du;' % (d, imm)]
            return ['R[%d] = 0x%Xu;' % (d, ((a + 4) & ~3) + imm)]
        if h & 0xFF00 == 0xB000:                        # add sp, #+-imm
            imm = (h & 0x7F) * 4
            return ['R[13] %s= %du;' % ('-' if (h >> 7) & 1 else '+', imm)]
        if h & 0xF600 == 0xB400:                        # push/pop
            regs = [i for i in range(8) if (h >> i) & 1]
            if (h >> 11) & 1:   # pop
                L = ['{ uint32_t addr = R[13];']
                for r in regs:
                    L.append('  R[%d] = rd32(addr); addr += 4;' % r)
                if (h >> 8) & 1:
                    L.append('  uint32_t newpc = rd32(addr); addr += 4; R[13] = addr; RET = newpc; return;')
                L.append('  R[13] = addr; }')
                return L
            if (h >> 8) & 1: regs.append(14)
            L = ['{ uint32_t addr = R[13] - %d; R[13] = addr;' % (4 * len(regs))]
            for r in regs:
                L.append('  wr32(addr, R[%d]); addr += 4;' % r)
            L.append('}')
            return L
        if h >> 12 == 0xC:                              # ldmia/stmia
            rb = (h >> 8) & 7
            regs = [i for i in range(8) if (h >> i) & 1]
            L = ['{ uint32_t addr = R[%d];' % rb]
            for r in regs:
                L.append(('  R[%d] = rd32(addr); addr += 4;' if (h >> 11) & 1 else '  wr32(addr, R[%d]); addr += 4;') % r)
            if not ((h >> 11) & 1 and rb in regs):
                L.append('  R[%d] = addr;' % rb)
            L.append('}')
            return L
        if h >> 12 == 0xD:
            c = (h >> 8) & 0xF
            if c == 0xF:
                return ['swi(0x%Xu);' % (h & 0xFF)]
            off = h & 0xFF
            if off & 0x80: off -= 256
            return ['if (%s) goto L_%X;' % (COND[c], a + 4 + off * 2)]
        if top5 == 0x1C:
            off = h & 0x7FF
            if off & 0x400: off -= 2048
            t = a + 4 + off * 2
            if t in self.f.tailcalls:
                return ['%s; return;' % self.call(t | 1)]
            return ['goto L_%X;' % t]
        if top5 == 0x1E and h2 is not None:
            off = ((h & 0x7FF) << 12) | ((h2 & 0x7FF) << 1)
            if off & 0x400000: off -= 1 << 23
            t = a + 4 + off
            tgt = (t & ~3) if h2 >> 11 == 0x1D else (t | 1)
            return ['R[14] = 0x%Xu;' % ((a + 4) | 1), '%s;' % self.call(tgt),
                    'if (RET != 0x%Xu) { divert(entry_lr, entry_sp); return; }' % ((a + 4) | 1)]
        if h & 0xFF00 == 0xBE00:
            return ['/* bkpt */']
        raise Unsupported('thumb %04x' % h)

    def lift(self, cname):
        out = ['void %s(void) {' % cname, '    TRACE_FN(0x%Xu);' % (self.f.entry | 1),
               '    uint32_t entry_lr = R[14], entry_sp = R[13]; (void)entry_lr; (void)entry_sp;', '    CHECK();']
        addrs = sorted(self.f.insns)
        starts, prev_end = [], None
        for a in addrs:
            if a in self.f.labels or a != prev_end:
                starts.append(a)
            prev_end = a + self.f.insns[a][1]
        blen = {}
        for i, b in enumerate(starts):
            end = starts[i + 1] if i + 1 < len(starts) else None
            blen[b] = len([x for x in addrs if x >= b and (end is None or x < end)])
        sset = set(starts)
        for a in addrs:
            h, size, h2 = self.f.insns[a]
            if a in sset:
                out.append('L_%X: TICK(%d); CHECK();' % (a, blen[a]))
            try:
                lines = self.body(a, h, h2)
            except Unsupported as e:
                self.unsupported.append((a, h, str(e)))
                lines = ['unsupported_insn(0x%Xu, 0x%04Xu);' % (a, h)]
            out += ['    ' + x for x in lines]
        out += ['    RET = R[14]; return;', '}']
        return '\n'.join(out)
