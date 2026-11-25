/* CPU state + memory interface used by recompiled Explorers of Sky code. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
extern uint32_t *R;          /* points at melonDS ARM9 register file */
extern int N, Z, C, V, SHC;
extern uint32_t RET;         /* address a native function returned to */
extern uint64_t *ARM9TS, *ARM9TG;
extern volatile uint8_t *IRQP;
void native_check(void);
extern uint32_t fn_ring[256]; extern unsigned fn_ring_pos;
#ifdef RECOMP_TRACE_FN
#define TRACE_FN(a) (fn_ring[fn_ring_pos++ & 255] = (a))
#else
#define TRACE_FN(a) ((void)0)
#endif
#define TICK(n) (*ARM9TS += 2 * (n))
#define CHECK() do { if (*ARM9TS >= *ARM9TG || *IRQP) native_check(); } while (0)

uint32_t rd8(uint32_t a);
uint32_t rd16(uint32_t a);
uint32_t rd32(uint32_t a);
void wr8(uint32_t a, uint32_t v);
void wr16(uint32_t a, uint32_t v);
void wr32(uint32_t a, uint32_t v);

static inline uint32_t ror32(uint32_t v, unsigned n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }
static inline uint32_t rd32_rot(uint32_t a) { return ror32(rd32(a & ~3u), (a & 3) * 8); }
static inline uint32_t rds8(uint32_t a) { return (uint32_t)(int32_t)(int8_t)rd8(a); }
static inline uint32_t rds16(uint32_t a) { return (uint32_t)(int32_t)(int16_t)rd16(a); }
static inline uint32_t clz32(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32; }

static inline void setNZ(uint32_t r) { N = r >> 31; Z = r == 0; }
static inline void setadd(uint32_t a, uint32_t b, uint32_t r) { setNZ(r); C = r < a; V = ((a ^ r) & (b ^ r)) >> 31; }
static inline void setadc(uint32_t a, uint32_t b, uint32_t r) { uint64_t t = (uint64_t)a + b + (uint32_t)C; setNZ(r); C = (int)(t >> 32); V = ((a ^ r) & (b ^ r)) >> 31; }
static inline void setsub(uint32_t a, uint32_t b, uint32_t r) { setNZ(r); C = a >= b; V = ((a ^ b) & (a ^ r)) >> 31; }
static inline void setsbc(uint32_t a, uint32_t b, uint32_t r) { int c = (uint64_t)a >= (uint64_t)b + (uint32_t)!C; setNZ(r); C = c; V = ((a ^ b) & (a ^ r)) >> 31; }

/* register-specified shifts; *_c variants also set SHC (shifter carry out) */
static inline uint32_t lsl_reg(uint32_t v, uint32_t s) { return s >= 32 ? 0 : v << s; }
static inline uint32_t lsr_reg(uint32_t v, uint32_t s) { return s >= 32 ? 0 : v >> s; }
static inline uint32_t asr_reg(uint32_t v, uint32_t s) { return (uint32_t)((int32_t)v >> (s >= 32 ? 31 : s)); }
static inline uint32_t ror_reg(uint32_t v, uint32_t s) { return ror32(v, s & 31); }
static inline uint32_t lsl_reg_c(uint32_t v, uint32_t s) { if (!s) { SHC = C; return v; } SHC = s > 32 ? 0 : (v >> (32 - s)) & 1; return lsl_reg(v, s); }
static inline uint32_t lsr_reg_c(uint32_t v, uint32_t s) { if (!s) { SHC = C; return v; } SHC = s > 32 ? 0 : (v >> (s - 1)) & 1; return lsr_reg(v, s); }
static inline uint32_t asr_reg_c(uint32_t v, uint32_t s) { if (!s) { SHC = C; return v; } SHC = s >= 32 ? v >> 31 : (v >> (s - 1)) & 1; return asr_reg(v, s); }
static inline uint32_t ror_reg_c(uint32_t v, uint32_t s) { if (!s) { SHC = C; return v; } s &= 31; uint32_t r = ror32(v, s); SHC = r >> 31; return r; }

uint32_t smla_q(uint32_t prod, uint32_t acc);
uint32_t qadd(uint32_t a, uint32_t b);
uint32_t qsub(uint32_t a, uint32_t b);
uint32_t qdadd(uint32_t a, uint32_t b);
uint32_t qdsub(uint32_t a, uint32_t b);
uint32_t read_psr(int spsr);
void write_psr(int spsr, int mask, uint32_t v);
void restore_spsr(void);
uint32_t mrc(int cp, int op1, int crn, int crm, int op2, int unused);
void mcr(int cp, int op1, int crn, int crm, int op2, uint32_t v);
extern uint32_t recomp_halt_pc;
void swi(uint32_t n);
void call_indirect(uint32_t addr);
void indirect_jump(uint32_t addr);
void call_addr(uint32_t addr);
void bad_jump(uint32_t addr);
void divert(uint32_t entry_lr, uint32_t entry_sp);
void unsupported_insn(uint32_t addr, uint32_t word);
#ifdef __cplusplus
}
#endif
