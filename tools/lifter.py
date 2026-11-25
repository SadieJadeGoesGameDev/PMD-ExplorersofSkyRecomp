"""ARMv5TE -> C lifter for the Explorers of Sky recomp (first pass).

Each recompiled function is a C function `void f_<bin>_<addr>(void)` operating on
the global CPU state `R[16]`, flags `N,Z,C,V`, with memory via rd32/wr32 etc.
"""
import struct

COND = ['Z', '!Z', 'C', '!C', 'N', '!N', 'V', '!V',
        '(C && !Z)', '(!C || Z)', '(N == V)', '(N != V)', '(!Z && N == V)', '(Z || N != V)', None, None]


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def ror(v, n):
    n &= 31
    return ((v >> n) | (v << (32 - n))) & 0xFFFFFFFF


class Unsupported(Exception):
    pass


class Func:
    """Discovers the instructions belonging to one ARM function by following control flow."""

    def __init__(self, binname, base, data, entry, known_starts):
        self.bin, self.base, self.data, self.entry = binname, base, data, entry
        self.known = known_starts
        self.insns = {}          # addr -> word
        self.labels = set()      # branch targets inside function
        self.jumptables = {}     # addr of dispatch insn -> list of target addrs
        self.calls = set()
        self.tailcalls = set()
        self.callret = {}        # addr of 'bx rN' acting as a call -> return address
        self.discover()

    def word(self, a):
        o = a - self.base
        if not (0 <= o <= len(self.data) - 4):
            raise Unsupported('outside binary %x' % a)
        return u32(self.data, o)

    def discover(self):
        todo = [self.entry]
        self.labels.add(self.entry)
        while todo:
            a = todo.pop()
            while a not in self.insns:
                w = self.word(a)
                self.insns[a] = w
                cond = w >> 28
                nxt = a + 4
                stop = False
                if cond == 0xF:
                    if (w >> 25) & 7 == 5:      # BLX imm
                        off = w & 0xFFFFFF
                        if off & 0x800000: off -= 1 << 24
                        self.calls.add((a + 8 + off * 4 + ((w >> 24) & 1) * 2) | 1)
                    # PLD etc: just continue
                elif (w >> 25) & 7 == 5:          # B / BL
                    off = w & 0xFFFFFF
                    if off & 0x800000: off -= 1 << 24
                    t = a + 8 + off * 4
                    if (w >> 24) & 1:
                        self.calls.add(t)
                    else:
                        if t != self.entry and t in self.known:
                            self.tailcalls.add(t)
                        else:
                            self.labels.add(t); todo.append(t)
                        if cond == 0xE: stop = True
                elif w & 0x0FFFFFF0 == 0x012FFF10:  # BX
                    ret = self.lr_setup(a) if (w & 0xF) != 14 else None
                    if ret is not None:
                        self.callret[a] = ret
                        self.labels.add(ret); todo.append(ret)
                        stop = True
                    elif cond == 0xE: stop = True
                elif self.writes_pc(w):
                    jt = self.jumptable(a, w)
                    if jt is not None:
                        self.jumptables[a] = jt
                        for t in jt:
                            self.labels.add(t); todo.append(t)
                        if cond == 0xE:
                            stop = True
                        else:
                            self.labels.add(a + 4)
                    elif cond == 0xE:
                        stop = True
                if stop:
                    break
                a = nxt
                self.labels.add(a) if cond not in (0xE, 0xF) and False else None

    def lr_setup(self, a):
        # look back a few instructions for 'ldr lr, [pc, #x]' or 'mov lr, pc' / 'add lr, pc, #x'
        for back in range(1, 5):
            p = self.insns.get(a - 4 * back)
            if p is None:
                return None
            pa = a - 4 * back
            if p & 0x0FFFF000 == 0x059FE000 or p & 0x0FFFF000 == 0x051FE000:   # ldr lr, [pc, #+-imm]
                off = p & 0xFFF
                lit = pa + 8 + (off if (p >> 23) & 1 else -off)
                v = self.word(lit)
                return v if not v & 1 and self.base <= v < self.base + len(self.data) else None
            if p & 0x0FFFFFFF == 0x01A0E00F:                                     # mov lr, pc
                return pa + 8
            if p & 0x0FFFF000 == 0x028FE000:                                     # add lr, pc, #imm
                return pa + 8 + ror(p & 0xFF, ((p >> 8) & 0xF) * 2)
            if (p >> 12) & 0xF == 14:
                return None                                                      # lr written otherwise
        return None

    @staticmethod
    def writes_pc(w):
        if (w >> 26) & 3 == 0 and not ((w >> 25) & 1 == 0 and (w >> 4) & 9 == 9):
            op = (w >> 21) & 0xF
            if op in (8, 9, 10, 11):       # tst/teq/cmp/cmn
                return False
            return (w >> 12) & 0xF == 15
        if (w >> 26) & 3 == 1 and (w >> 20) & 1:  # LDR
            return (w >> 12) & 0xF == 15
        if (w >> 25) & 7 == 4 and (w >> 20) & 1:  # LDM
            return bool(w & 0x8000)
        return False

    def jumptable(self, a, w):
        # add<cond> pc, pc, rX, lsl #2 ; then a table of unconditional branches at a+8...
        if w & 0x0FFFFFF0 != 0x008FF100:
            return None
        rx = w & 0xF
        n_cmp = 0
        if w >> 28 == 0x9:                                      # addls: the nearest cmp is the upper bound
            for back in range(1, 12):
                p = self.insns.get(a - 4 * back)
                if p is None:
                    break
                if p & 0x0FFFF000 == 0x03500000 | (rx << 16):      # cmp rX, #imm
                    n_cmp = ror(p & 0xFF, ((p >> 8) & 0xF) * 2) + 1
                    break
        else:
            # e.g. "cmp rX,#16; bgt out; cmp rX,#0; addge pc,..": the bound is the cmp followed by a b<gt|hi|ge|cs>
            for back in range(1, 12):
                p = self.insns.get(a - 4 * back)
                if p is None:
                    break
                if p & 0x0FFFF000 == 0x03500000 | (rx << 16):
                    nx = self.insns.get(a - 4 * back + 4)
                    if nx is not None and (nx >> 24) & 0xF == 0xA and (nx >> 28) in (0xC, 0x8, 0xA, 0x2):
                        imm = ror(p & 0xFF, ((p >> 8) & 0xF) * 2)
                        n_cmp = imm + 1 if (nx >> 28) in (0xC, 0x8) else imm
                        break
        n = 0
        while n < 512:
            try:
                t = self.word(a + 8 + 4 * n)
            except Unsupported:
                break
            if t >> 24 != 0xEA:
                break
            n += 1
        n = max(n, n_cmp)
        if n == 0:
            raise Unsupported('empty jumptable at %x' % a)
        # entries jump to the branch instructions themselves
        return [a + 8 + 4 * i for i in range(n)]


class Lifter:
    def __init__(self, func, name_of):
        self.f = func
        self.name_of = name_of   # addr -> C function name for calls
        self.out = []
        self.unsupported = []

    def reg(self, n, a, extra=0):
        if n == 15:
            return '0x%Xu' % ((a + 8 + extra) & 0xFFFFFFFF)
        return 'R[%d]' % n

    @staticmethod
    def check_ret(exp):
        return 'if (RET != 0x%Xu) { divert(entry_lr, entry_sp); return; }' % exp

    def call(self, t):
        n = self.name_of(t, self.f)
        return n if n.startswith('CALL_ADDR') else n + '()'

    def emit(self, s):
        self.out.append(s)

    def shifter(self, w, a, setc):
        """Returns (expr, carry_expr_or_None) for the data-processing operand 2."""
        if (w >> 25) & 1:
            rot = ((w >> 8) & 0xF) * 2
            v = ror(w & 0xFF, rot)
            carry = None
            if setc and rot:
                carry = '%d' % (v >> 31)
            return '0x%Xu' % v, carry
        rm = w & 0xF
        typ = (w >> 5) & 3
        if (w >> 4) & 1:   # register shift
            rs = (w >> 8) & 0xF
            m = self.reg(rm, a, 4)
            s = '(R[%d] & 0xFF)' % rs
            fn = ['lsl_reg', 'lsr_reg', 'asr_reg', 'ror_reg'][typ]
            if setc:
                return '%s_c(%s, %s)' % (fn, m, s), 'SHC'
            return '%s(%s, %s)' % (fn, m, s), None
        amt = (w >> 7) & 0x1F
        m = self.reg(rm, a)
        if typ == 0:
            if amt == 0:
                return m, None
            return '(%s << %d)' % (m, amt), ('((%s >> %d) & 1)' % (m, 32 - amt) if setc else None)
        if typ == 1:
            if amt == 0:
                return '0u', ('(%s >> 31)' % m if setc else None)
            return '(%s >> %d)' % (m, amt), ('((%s >> %d) & 1)' % (m, amt - 1) if setc else None)
        if typ == 2:
            if amt == 0:
                return '((uint32_t)((int32_t)%s >> 31))' % m, ('(%s >> 31)' % m if setc else None)
            return '((uint32_t)((int32_t)%s >> %d))' % (m, amt), ('((%s >> %d) & 1)' % (m, amt - 1) if setc else None)
        if amt == 0:   # RRX
            return '((%s >> 1) | ((uint32_t)C << 31))' % m, ('(%s & 1)' % m if setc else None)
        return 'ror32(%s, %d)' % (m, amt), ('((%s >> %d) & 1)' % (m, amt - 1) if setc else None)

    def lift_insn(self, a, w):
        cond = w >> 28
        body = []
        try:
            body = self.lift_body(a, w)
        except Unsupported as e:
            self.unsupported.append((a, w, str(e)))
            body = ['unsupported_insn(0x%Xu, 0x%08Xu);' % (a, w)]
        c = COND[cond]
        if c is None:
            return body
        return ['if (%s) {' % c] + ['    ' + x for x in body] + ['}']

    def ret_from_pc(self):
        return ['return;']

    def lift_body(self, a, w):
        L = []
        cond = w >> 28
        if cond == 0xF:
            if (w >> 25) & 7 == 5:
                off = w & 0xFFFFFF
                if off & 0x800000: off -= 1 << 24
                t = (a + 8 + off * 4 + ((w >> 24) & 1) * 2)
                return ['R[14] = 0x%Xu;' % (a + 4), '%s;' % self.call(t | 1), self.check_ret(a + 4)]
            if w & 0xFD70F000 == 0xF550F000:   # PLD
                return ['/* pld */']
            raise Unsupported('cond F')
        # BX / BLX reg
        if w & 0x0FFFFFF0 == 0x012FFF10:
            rm = w & 0xF
            if rm == 14:
                return ['RET = R[14]; return;']
            if a in self.f.callret:
                return ['call_indirect(%s);' % self.reg(rm, a), self.check_ret(self.f.callret[a]), 'goto L_%X;' % self.f.callret[a]]
            return ['indirect_jump(%s); return;' % self.reg(rm, a)]
        if w & 0x0FFFFFF0 == 0x012FFF30:
            rm = w & 0xF
            return ['{ uint32_t t = %s; R[14] = 0x%Xu; call_indirect(t); }' % (self.reg(rm, a), a + 4), self.check_ret(a + 4)]
        # B / BL
        if (w >> 25) & 7 == 5:
            off = w & 0xFFFFFF
            if off & 0x800000: off -= 1 << 24
            t = a + 8 + off * 4
            if (w >> 24) & 1:
                return ['R[14] = 0x%Xu;' % (a + 4), '%s;' % self.call(t), self.check_ret(a + 4)]
            if t in self.f.tailcalls:
                return ['%s; return;' % self.call(t)]
            return ['goto L_%X;' % t]
        # CLZ
        if w & 0x0FFF0FF0 == 0x016F0F10:
            return ['R[%d] = clz32(R[%d]);' % ((w >> 12) & 0xF, w & 0xF)]
        # MRS / MSR
        if w & 0x0FBF0FFF == 0x010F0000:
            return ['R[%d] = read_psr(%d);' % ((w >> 12) & 0xF, (w >> 22) & 1)]
        if w & 0x0DB0F000 == 0x0120F000:
            mask = (w >> 16) & 0xF
            if (w >> 25) & 1:
                v = '0x%Xu' % ror(w & 0xFF, ((w >> 8) & 0xF) * 2)
            else:
                v = 'R[%d]' % (w & 0xF)
            return ['write_psr(%d, %d, %s);' % ((w >> 22) & 1, mask, v)]
        # SWP
        if w & 0x0FB00FF0 == 0x01000090:
            b = (w >> 22) & 1; rn, rd, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, w & 0xF
            sz = '8' if b else '32'
            return ['{ uint32_t t = rd%s(R[%d]); wr%s(R[%d], R[%d]); R[%d] = t; }' % (sz, rn, sz, rn, rm, rd)]
        # multiplies
        if w & 0x0F0000F0 == 0x00000090 and (w >> 24) & 0xF == 0:
            return self.mul(a, w)
        # DSP multiplies / saturating
        if w & 0x0F900090 == 0x01000080:
            return self.dsp_mul(a, w)
        if w & 0x0F900FF0 == 0x01000050:
            op = (w >> 21) & 3; rn, rd, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, w & 0xF
            fn = ['qadd', 'qsub', 'qdadd', 'qdsub'][op]
            return ['R[%d] = %s(R[%d], R[%d]);' % (rd, fn, rm, rn)]
        # halfword / signed / doubleword transfers
        if (w >> 25) & 7 == 0 and (w >> 4) & 9 == 9 and (w >> 5) & 3:
            return self.halfword(a, w)
        if (w >> 26) & 3 == 0:
            return self.dataproc(a, w)
        if (w >> 26) & 3 == 1:
            if (w >> 25) & 1 and (w >> 4) & 1:
                raise Unsupported('undefined')
            return self.ldst(a, w)
        if (w >> 25) & 7 == 4:
            return self.ldm(a, w)
        if (w >> 24) & 0xF == 0xF:
            return ['swi(0x%Xu);' % ((w >> 16) & 0xFF)]   # BIOS reads comment bits 16-23 in ARM state
        if (w >> 24) & 0xF == 0xE and (w >> 4) & 1:
            l = (w >> 20) & 1
            opc1, crn, rd, cp, opc2, crm = (w >> 21) & 7, (w >> 16) & 0xF, (w >> 12) & 0xF, (w >> 8) & 0xF, (w >> 5) & 7, w & 0xF
            if l:
                return ['R[%d] = mrc(%d,%d,%d,%d,%d,%d);' % (rd, cp, opc1, crn, crm, opc2, 0)]
            if cp == 15 and crn == 7 and crm == 0 and opc2 == 4:   # wait for interrupt: remember where (quicksaves resume here)
                return ['recomp_halt_pc = 0x%Xu;' % a, 'mcr(%d,%d,%d,%d,%d,R[%d]);' % (cp, opc1, crn, crm, opc2, rd)]
            return ['mcr(%d,%d,%d,%d,%d,R[%d]);' % (cp, opc1, crn, crm, opc2, rd)]
        raise Unsupported('unknown')

    def mul(self, a, w):
        op = (w >> 21) & 0xF
        s = (w >> 20) & 1
        rd, rn, rs, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, (w >> 8) & 0xF, w & 0xF
        L = []
        if op == 0:
            L.append('R[%d] = R[%d] * R[%d];' % (rd, rm, rs))
            if s: L.append('setNZ(R[%d]);' % rd)
        elif op == 1:
            L.append('R[%d] = R[%d] * R[%d] + R[%d];' % (rd, rm, rs, rn))
            if s: L.append('setNZ(R[%d]);' % rd)
        elif op in (4, 5, 6, 7):
            hi, lo = rd, rn
            signed = op in (6, 7)
            t = 'int64_t' if signed else 'uint64_t'
            c = 'int32_t' if signed else 'uint32_t'
            expr = '(%s)(%s)R[%d] * (%s)(%s)R[%d]' % (t, c, rm, t, c, rs)
            if op & 1:
                expr += ' + (%s)(((uint64_t)R[%d] << 32) | R[%d])' % (t, hi, lo)
            L.append('{ uint64_t t = (uint64_t)(%s); R[%d] = (uint32_t)t; R[%d] = (uint32_t)(t >> 32);' % (expr, lo, hi)
                     + (' N = (t >> 63) & 1; Z = (t == 0);' if s else '') + ' }')
        else:
            raise Unsupported('mul op')
        return L

    def dsp_mul(self, a, w):
        op = (w >> 21) & 3
        rd, rn, rs, rm = (w >> 16) & 0xF, (w >> 12) & 0xF, (w >> 8) & 0xF, w & 0xF
        x, y = (w >> 5) & 1, (w >> 6) & 1
        hx = '(int32_t)(int16_t)(R[%d] >> %d)' % (rm, 16 * x)
        hy = '(int32_t)(int16_t)(R[%d] >> %d)' % (rs, 16 * y)
        if op == 0:
            return ['R[%d] = smla_q((int32_t)(%s * %s), R[%d]);' % (rd, hx, hy, rn)]
        if op == 1:
            prod = '(uint32_t)(((int64_t)(int32_t)R[%d] * (int64_t)%s) >> 16)' % (rm, hy)
            if x == 0:
                return ['R[%d] = smla_q(%s, R[%d]);' % (rd, prod, rn)]
            return ['R[%d] = %s;' % (rd, prod)]
        if op == 2:
            return ['{ int64_t t = (int64_t)(((uint64_t)R[%d] << 32) | R[%d]) + (int64_t)(%s * %s); R[%d] = (uint32_t)t; R[%d] = (uint32_t)((uint64_t)t >> 32); }'
                    % (rd, rn, hx, hy, rn, rd)]
        return ['R[%d] = (uint32_t)(%s * %s);' % (rd, hx, hy)]

    def dataproc(self, a, w):
        op = (w >> 21) & 0xF
        s = (w >> 20) & 1
        rn, rd = (w >> 16) & 0xF, (w >> 12) & 0xF
        regshift = not (w >> 25) & 1 and (w >> 4) & 1
        logical = op in (0, 1, 8, 9, 12, 13, 14, 15)
        op2, carry = self.shifter(w, a, s and logical)
        n = self.reg(rn, a, 4 if regshift else 0)
        L = []
        res = {0: '%s & %s', 1: '%s ^ %s', 2: '%s - %s', 3: '%s - %s', 4: '%s + %s',
               5: '%s + %s + C', 6: '%s - %s - !C', 7: '%s - %s - !C', 8: '%s & %s', 9: '%s ^ %s',
               10: '%s - %s', 11: '%s + %s', 12: '%s | %s', 13: '%s', 14: '%s & ~%s', 15: '~%s'}
        if op in (3, 7):
            expr = res[op] % ('op2', 'rn')
        elif op == 13:
            expr = 'op2'
        elif op == 15:
            expr = '~op2'
        else:
            expr = res[op] % ('rn', 'op2')
        L.append('{ uint32_t op2 = %s; uint32_t rn = %s; (void)rn; uint32_t res = %s;' % (op2, n, expr))
        if s:
            if rd == 15:
                L.append('  restore_spsr();')
            if logical:
                if carry == 'SHC':
                    L.append('  C = SHC;')
                elif carry is not None:
                    L.append('  C = %s;' % carry)
                L.append('  setNZ(res);')
            elif op in (4, 11):
                L.append('  setadd(rn, op2, res);')
            elif op == 5:
                L.append('  setadc(rn, op2, res);')
            elif op in (2, 10):
                L.append('  setsub(rn, op2, res);')
            elif op == 3:
                L.append('  setsub(op2, rn, res);')
            elif op == 6:
                L.append('  setsbc(rn, op2, res);')
            elif op == 7:
                L.append('  setsbc(op2, rn, res);')
        if op not in (8, 9, 10, 11):
            if rd == 15:
                if a in self.f.jumptables:
                    L.append('  switch (res) {')
                    for t in self.f.jumptables[a]:
                        L.append('    case 0x%Xu: goto L_%X;' % (t, t))
                    L.append('    default: bad_jump(res);')
                    L.append('  }')
                elif op == 13 and (w & 0xF) == 14 and not (w >> 25) & 1:
                    L.append('  RET = res; return; /* mov pc, lr */')
                else:
                    L.append('  indirect_jump(res); return;')
            else:
                L.append('  R[%d] = res;' % rd)
        L.append('}')
        return L

    def ldst(self, a, w):
        I, P, U, B, W, Ld = [(w >> k) & 1 for k in (25, 24, 23, 22, 21, 20)]
        rn, rd = (w >> 16) & 0xF, (w >> 12) & 0xF
        if I:
            amt, typ, rm = (w >> 7) & 0x1F, (w >> 5) & 3, w & 0xF
            m = self.reg(rm, a)
            if typ == 0: off = '(%s << %d)' % (m, amt)
            elif typ == 1: off = '(%s >> %d)' % (m, amt) if amt else '0u'
            elif typ == 2: off = '((uint32_t)((int32_t)%s >> %d))' % (m, amt if amt else 31)
            else: off = 'ror32(%s, %d)' % (m, amt) if amt else '((%s >> 1) | ((uint32_t)C << 31))' % m
        else:
            off = '0x%Xu' % (w & 0xFFF)
        sign = '+' if U else '-'
        base = self.reg(rn, a)
        if rn == 15 and not I and P:
            addr = '0x%Xu' % ((a + 8 + (w & 0xFFF) * (1 if U else -1)) & 0xFFFFFFFF)
        else:
            addr = '(%s %s %s)' % (base, sign, off) if P else base
        sz = '8' if B else '32'
        L = ['{ uint32_t addr = %s;' % addr]
        if Ld:
            L.append('  uint32_t v = rd%s%s(addr);' % (sz, '' if B else '_rot'))
        else:
            L.append('  wr%s(addr, %s);' % (sz, self.reg(rd, a, 4)))
        if not P:
            L.append('  R[%d] = R[%d] %s %s;' % (rn, rn, sign, off))
        elif W:
            L.append('  R[%d] = addr;' % rn)
        if Ld:
            if rd == 15:
                if rn == 13:
                    L.append('  RET = v; return;   /* ldr pc, [sp] = pop return */')
                else:
                    L.append('  indirect_jump(v); return;')
            else:
                L.append('  R[%d] = v;' % rd)
        L.append('}')
        return L

    def halfword(self, a, w):
        P, U, I, W, Ld = [(w >> k) & 1 for k in (24, 23, 22, 21, 20)]
        rn, rd = (w >> 16) & 0xF, (w >> 12) & 0xF
        sh = (w >> 5) & 3
        off = '0x%Xu' % (((w >> 4) & 0xF0) | (w & 0xF)) if I else 'R[%d]' % (w & 0xF)
        sign = '+' if U else '-'
        base = self.reg(rn, a)
        addr = '(%s %s %s)' % (base, sign, off) if P else base
        L = ['{ uint32_t addr = %s;' % addr]
        post = []
        if not P:
            post.append('  R[%d] = R[%d] %s %s;' % (rn, rn, sign, off))
        elif W:
            post.append('  R[%d] = addr;' % rn)
        if Ld:
            fn = {1: 'rd16', 2: 'rds8', 3: 'rds16'}[sh]
            L.append('  uint32_t v = %s(addr);' % fn)
            L += post
            L.append('  R[%d] = v;' % rd)
        else:
            if sh == 1:
                L.append('  wr16(addr, %s);' % self.reg(rd, a, 4))
            elif sh == 2:   # LDRD
                L.append('  uint32_t v0 = rd32(addr), v1 = rd32(addr + 4);')
                L += post
                L.append('  R[%d] = v0; R[%d] = v1;' % (rd, rd + 1))
                L.append('}')
                return L
            else:           # STRD
                L.append('  wr32(addr, R[%d]); wr32(addr + 4, R[%d]);' % (rd, rd + 1))
            L += post
        L.append('}')
        return L

    def ldm(self, a, w):
        P, U, S, W, Ld = [(w >> k) & 1 for k in (24, 23, 22, 21, 20)]
        rn = (w >> 16) & 0xF
        regs = [i for i in range(16) if (w >> i) & 1]
        n = len(regs)
        if U:
            start = '(R[%d] + 4)' % rn if P else 'R[%d]' % rn
            wb = 'R[%d] + %d' % (rn, 4 * n)
        else:
            start = '(R[%d] - %d)' % (rn, 4 * n) if P else '(R[%d] - %d)' % (rn, 4 * n - 4)
            wb = 'R[%d] - %d' % (rn, 4 * n)
        L = ['{ uint32_t addr = %s; uint32_t wbv = %s;' % (start, wb)]
        if Ld:
            for i, r in enumerate(regs):
                if r == 15:
                    L.append('  uint32_t newpc = rd32(addr + %d);' % (4 * i))
                else:
                    L.append('  uint32_t v%d = rd32(addr + %d);' % (r, 4 * i))
            if W and rn not in regs:
                L.append('  R[%d] = wbv;' % rn)
            for r in regs:
                if r != 15:
                    L.append('  R[%d] = v%d;' % (r, r))
            if 15 in regs:
                L.append('  (void)wbv; RET = newpc; return;')
        else:
            for i, r in enumerate(regs):
                L.append('  wr32(addr + %d, %s);' % (4 * i, self.reg(r, a, 4) if r == 15 else 'R[%d]' % r))
            if W:
                L.append('  R[%d] = wbv;' % rn)
        L.append('  (void)wbv; }')
        return L

    def lift(self, cname):
        f = self.f
        self.emit('void %s(void) {' % cname)
        self.emit('    TRACE_FN(0x%Xu);' % f.entry)
        self.emit('    uint32_t entry_lr = R[14], entry_sp = R[13]; (void)entry_lr; (void)entry_sp;')
        self.emit('    CHECK();')
        addrs = sorted(f.insns)
        # block starts: labels, or discontinuities
        starts = set()
        prev = None
        for a in addrs:
            if a in f.labels or prev is None or a != prev + 4:
                starts.add(a)
            prev = a
        sl = sorted(starts)
        blen = {}
        for i, b in enumerate(sl):
            end = sl[i + 1] if i + 1 < len(sl) else None
            blen[b] = len([x for x in addrs if x >= b and (end is None or x < end)])
        for a in addrs:
            if a in starts:
                self.emit('L_%X: TICK(%d); CHECK();' % (a, blen[a]))
            for line in self.lift_insn(a, f.insns[a]):
                self.emit('    ' + line)
        self.emit('    RET = R[14]; return;')
        self.emit('}')
        return '\n'.join(self.out)
