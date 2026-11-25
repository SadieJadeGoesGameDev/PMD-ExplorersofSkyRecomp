// Hybrid runtime: recompiled (native) ARM9 code running inside melonDS's hardware.
//
// Each game thread runs on its own host fiber. When melonDS's scheduler asks the ARM9
// to run up to ARM9Target, we switch into the current game fiber; native code charges
// cycles (TICK) and switches back (CHECK) once the budget is used up, or takes an IRQ.
// Code we don't have natively (BIOS, OS_LoadContext, odd jumps) runs on melonDS's
// interpreter one instruction at a time, calling native code whenever it reaches a
// function we have. Thread switches show up as exception returns that land somewhere
// other than where the current fiber expects; we then switch to the fiber parked at
// that resume point (or start a new one).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <map>
#include <unordered_map>
#include <algorithm>
#include "NDS.h"
#include "ARM.h"
extern "C" {
#include "cpu.h"
#include "dispatch.h"
}

#ifdef _WIN32
#include <windows.h>
#else
#include <ucontext.h>
#endif

using namespace melonDS;

extern "C" {
uint32_t *R;
int N, Z, C, V, SHC;
uint32_t RET;
uint64_t *ARM9TS, *ARM9TG;
volatile uint8_t *IRQP;
uint32_t fn_ring[256];
unsigned fn_ring_pos;
}

static ARMv5 *cpu;
extern "C" { uint32_t recomp_halt_pc; }
static uint32_t halt_safe_pc;
static NDS *nds;
static const uint32_t MAGIC = 0x01FFFF00;   // fake return address for IRQ/SWI entry (unused ITCM)
static const uint32_t SAVECTX_RESUME = 0x02079D4C;
static bool trace = getenv("RECOMP_TRACE") != nullptr;
static uint64_t stat_native_calls, stat_interp_steps, stat_fallbacks, stat_fibers, stat_switches, stat_diverts;
static size_t stat_stack_max;   // deepest host stack use seen on a game fiber since the last report
struct Fiber;
static inline void stack_probe();
static std::unordered_map<uint32_t, uint64_t> fallback_hist;

static void dump_ring() {
    fprintf(stderr, "[recomp] last native functions:\n");
    for (unsigned i = 0; i < 40; i++) {
        unsigned k = (fn_ring_pos - 40 + i) & 255;
        fprintf(stderr, " %08X", fn_ring[k]);
        if (i % 8 == 7) fprintf(stderr, "\n");
    }
}

// ---------------------------------------------------------------- fibers
typedef std::pair<uint32_t, uint32_t> Key;          // (resume pc, resume sp)
struct Fiber {
#ifdef _WIN32
    LPVOID h = nullptr;
#else
    ucontext_t ctx;
    char *stack = nullptr;
#endif
    std::vector<Key> keys;                          // resume points this thread is waiting on
};
static Fiber emu_f, *cur;
static inline void stack_probe() {
    char probe; size_t used;
#ifdef _WIN32
    used = (size_t)((char *)((NT_TIB *)NtCurrentTeb())->StackBase - &probe);
#else
    if (!cur || !cur->stack) return;
    used = (size_t)(cur->stack + (16 << 20) - &probe);
#endif
    if (used > stat_stack_max && used < (64u << 20)) stat_stack_max = used;
}
static std::map<Key, Fiber *> waiting;              // parked fibers, by where their thread resumes
static const size_t FIBER_STACK = 16 << 20;

static void fiber_main();
static std::vector<Fiber *> graveyard;     // fibers abandoned by a soft reset, freed by the next fiber
static uint64_t stat_resets;
static uint32_t swi_hist[16][3]; static unsigned swi_hist_pos;   // n, lr, sp of recent native SWIs
int recomp_post_reset_log = 0;              // >0: log fiber switches/natives for a while after a reset
#ifdef _WIN32
static void WINAPI fiber_entry(LPVOID) { fiber_main(); }
#endif

static Fiber *new_fiber() {
    Fiber *f = new Fiber;
    stat_fibers++;
    if (recomp_post_reset_log > 0) fprintf(stderr, "[post-reset] new fiber #%llu\n", (unsigned long long)stat_fibers);
#ifdef _WIN32
    // commit a little, reserve the full stack (CreateFiber would commit all of it up front)
    f->h = CreateFiberEx(256 << 10, FIBER_STACK, FIBER_FLAG_FLOAT_SWITCH, fiber_entry, nullptr);
    if (!f->h) { fprintf(stderr, "[recomp] CreateFiberEx failed (%lu) after %llu fibers\n", GetLastError(), (unsigned long long)stat_fibers); abort(); }
#else
    f->stack = (char *)malloc(FIBER_STACK);
    getcontext(&f->ctx);
    f->ctx.uc_stack.ss_sp = f->stack;
    f->ctx.uc_stack.ss_size = FIBER_STACK;
    f->ctx.uc_link = nullptr;
    makecontext(&f->ctx, fiber_main, 0);
#endif
    return f;
}

static void switch_fiber(Fiber *to) {
    Fiber *from = cur;
    cur = to;
#ifdef _WIN32
    (void)from;
    SwitchToFiber(to->h);
#else
    swapcontext(&from->ctx, &to->ctx);
#endif
}
static void switch_to_native() {
#ifdef _WIN32
    SwitchToFiber(cur->h);
#else
    swapcontext(&emu_f.ctx, &cur->ctx);
#endif
}
static void switch_to_emu() {
#ifdef _WIN32
    SwitchToFiber(emu_f.h);
#else
    swapcontext(&cur->ctx, &emu_f.ctx);
#endif
}

// ---------------------------------------------------------------- flags
static inline void pack_flags() {
    cpu->CPSR = (cpu->CPSR & 0x0FFFFFFF) | ((uint32_t)N << 31) | ((uint32_t)Z << 30) | ((uint32_t)C << 29) | ((uint32_t)V << 28);
}
static inline void unpack_flags() {
    uint32_t c = cpu->CPSR;
    N = (c >> 31) & 1; Z = (c >> 30) & 1; C = (c >> 29) & 1; V = (c >> 28) & 1;
}

// ---------------------------------------------------------------- lookup
static std::unordered_map<uint32_t, std::vector<const NativeFunc *>> table;

#include "settings.h"
static inline bool sadie_mask(unsigned m) {
    for (int g = 0; g < SADIE_COUNT; g++) if ((m >> g & 1) && recomp_settings.sadie[g]) return true;
    return false;
}
static bool overlay_loaded(int ov) {
    const OverlayFP &f = OVERLAY_FP[ov];
    for (int i = 0; i < f.n; i++)
        if (rd32(f.w[i].addr) != f.w[i].word) return false;
    return true;
}

static const NativeFunc *lookup(uint32_t addr) {
    auto it = table.find(addr);
    if (it == table.end()) return nullptr;
    for (const NativeFunc *nf : it->second) {
        if (nf->sadie && sadie_mask(nf->sadie)) return nullptr;     // Sadie Edition patch active: run the patched RAM
        if (nf->ov < 0) return nf;
        if (!nf->thumb && rd32(addr) != nf->sig) continue;
        if (overlay_loaded(nf->ov)) return nf;
    }
    return nullptr;
}

// ---------------------------------------------------------------- soft reset
// melonDS's free BIOS has no working ARM9 soft reset (it uses ARM7 addresses and trips the
// game's memory protection), which Explorers of Sky needs after every quicksave. When the
// game jumps there, restart its code natively from the entry point instead, on a fresh fiber.
static const uint32_t FREEBIOS_ARM9_RESET = 0xFFFF06FC;
static const uint32_t ARM9_ENTRY = 0x02000800;
static bool booted = false;
static inline bool is_reset_target(uint32_t a) { a &= ~1u; return a == FREEBIOS_ARM9_RESET || (booted && a == ARM9_ENTRY); }
static void reset_world() {
    stat_resets++;
    booted = false;                    // the fresh fiber's first visit to the entry point is the boot
    uint32_t entry = rd32(0x027FFE24);
    if (entry < 0x02000000 || entry >= 0x02400000) entry = 0x02000800;
    fprintf(stderr, "[recomp] soft reset -> restarting game code at %08X (cpsr %08X, %zu parked fibers)\n", entry, cpu->CPSR, waiting.size());
    uint32_t old = cpu->CPSR;
    uint32_t nw = (old & ~0xFFu) | 0xD3u;          // SVC, IRQ/FIQ off, ARM (like the BIOS hands over)
    cpu->CPSR = nw;
    if ((old & 0x1F) != 0x13) cpu->UpdateMode(old, nw);
    for (int i = 0; i < 13; i++) cpu->R[i] = 0;
    cpu->R[14] = 0;
    cpu->Halted = 0;
    cpu->JumpTo(entry);
    // every parked fiber belongs to the old run of the game
    for (auto &kv : waiting) graveyard.push_back(kv.second);
    waiting.clear();
    graveyard.push_back(cur);
    Fiber *f = new_fiber();
    switch_fiber(f);                               // never comes back
}

// The real ARM9 BIOS parks the CPU here while the ARM7 reloads the game from the card; the
// ARM7 then writes the new ARM9 entry point to 0x027FFE24. Wait for that, letting the rest
// of the system run, before restarting.
static void wait_for_arm7_reboot() {
    uint32_t start = rd32(0x027FFE24);
    fprintf(stderr, "[recomp] ARM9 parked in BIOS reset; waiting for ARM7 (entry slot %08X)\n", start);
    fprintf(stderr, "  how we got here: mode %02X lr=%08X sp=%08X spsr_svc=%08X lr_svc=%08X\n", cpu->CPSR & 0x1F, cpu->R[14], cpu->R[13], cpu->R_SVC[2], cpu->R_SVC[1]);
    fprintf(stderr, "  recent native SWIs (n lr sp):");
    for (unsigned i = 0; i < 16; i++) { uint32_t *h = swi_hist[(swi_hist_pos + i) & 15]; fprintf(stderr, " [%X %08X %08X]", h[0], h[1], h[2]); }
    fprintf(stderr, "\n  ring:");
    for (unsigned i = 0; i < 48; i++) fprintf(stderr, " %08X", fn_ring[(fn_ring_pos - 48 + i) & 255]);
    fprintf(stderr, "\n  ITCM swi word at trampoline: %08X\n", rd32(MAGIC - 4));
    for (int slices = 0;; slices++) {
        uint32_t e = rd32(0x027FFE24);
        if (e >= 0x02000000 && e < 0x02400000 && (e != start || slices > 2000000)) {
            fprintf(stderr, "[recomp] ARM7 finished reboot after %d slices, entry %08X\n", slices, e);
            return;
        }
        nds->ARM9Timestamp = nds->ARM9Target;
        switch_to_emu();
    }
}

// ---------------------------------------------------------------- interpreter loop
static void take_irq();
static void handle_halt(bool native = false) {
    if (cpu->Halted == 2) { cpu->Halted = 0; return; }
    if (nds->HaltInterrupted(0)) {
        cpu->Halted = 0;
        if (native) {                       // native code: take the IRQ via the trampoline
            if ((nds->IME[0] & 1) && !(cpu->CPSR & 0x80)) take_irq();
            return;
        }
        if (nds->IME[0] & 1) {
            uint32_t oldmode = cpu->CPSR & 0x1F;
            cpu->TriggerIRQ();
            uint32_t newmode = cpu->CPSR & 0x1F;
            if (oldmode != newmode && (oldmode == 0x10 || oldmode == 0x1F))
                cur->keys.push_back(Key(cpu->R[14] - 4, cpu->R_IRQ[0]));
        }
        return;
    }
    nds->ARM9Timestamp = nds->ARM9Target;
    switch_to_emu();
}

static inline uint32_t cur_pc() { return cpu->R[15] - ((cpu->CPSR & 0x20) ? 2 : 4); }
static inline bool thread_mode(uint32_t m) { m &= 0x1F; return m == 0x10 || m == 0x1F; }
static inline bool exc_mode(uint32_t m) { m &= 0x1F; return m == 0x12 || m == 0x13; }

static bool is_exc_return(uint32_t w) {
    // data-processing with S and Rd=pc (subs pc, lr / movs pc, lr), or LDM with ^ and pc in list
    if (((w >> 26) & 3) == 0 && ((w >> 20) & 1) && ((w >> 12) & 0xF) == 15) return true;
    if (((w >> 25) & 7) == 4 && ((w >> 20) & 1) && ((w >> 22) & 1) && (w & 0x8000)) return true;
    return false;
}

// An exception return landed at (pc, sp). If that isn't where this fiber expects to
// resume, the game switched threads: hand over to the fiber parked there (or a new one).
// Returns true if this fiber was parked and has now been resumed.
static bool on_exc_return() {
    Key t(cur_pc(), cpu->R[13]);
    Fiber *me = cur;
    if (!me->keys.empty() && me->keys.back() == t) { me->keys.pop_back(); return false; }
    Fiber *target;
    auto it = waiting.find(t);
    bool parked = it != waiting.end();
    if (parked) { target = it->second; waiting.erase(it); }
    else target = new_fiber();
    stat_switches++;
    if (trace || recomp_post_reset_log > 0) fprintf(stderr, "[recomp] thread switch -> pc %08X sp %08X (%s)\n", t.first, t.second, parked ? "parked fiber" : "new fiber");
    if (me->keys.empty()) fprintf(stderr, "[recomp] fiber with no resume point abandoned\n");
    else waiting[me->keys.back()] = me;
    switch_fiber(target);
    me->keys.pop_back();          // resumed: CPU is at our resume point
    return true;
}

// Interpret (calling native functions where possible) until PC == stop with SP == stop_sp.
// Returns true if diverted: this fiber was parked and resumed somewhere other than `stop`.
static bool run_interp(uint32_t stop, uint32_t stop_sp, bool has_stop) {
    for (;;) {
        if (cpu->Halted) { handle_halt(); continue; }
        uint32_t pc = cur_pc();
        if (has_stop && pc == stop && cpu->R[13] == stop_sp) return false;
        if (pc == FREEBIOS_ARM9_RESET) { wait_for_arm7_reboot(); reset_world(); continue; }
        if (is_reset_target(pc)) { reset_world(); continue; }
        if (pc == ARM9_ENTRY) booted = true;
        if (const NativeFunc *nf = lookup(pc)) {
            stat_native_calls++; stack_probe();
            if (recomp_post_reset_log > 0) { recomp_post_reset_log--; fprintf(stderr, "[post-reset] native %08X sp=%08X lr=%08X cpsr=%08X\n", pc, R[13], R[14], cpu->CPSR); }
            if (trace) fprintf(stderr, "[recomp] native %08X enter sp=%08X lr=%08X cpsr=%08X\n", pc, R[13], R[14], cpu->CPSR);
            unpack_flags();
            nf->fn();
            if (RET < 0x01000000) {
                fprintf(stderr, "[recomp] native %08X returned to %08X (sp=%08X cpsr=%08X)\n", pc, RET, cpu->R[13], cpu->CPSR);
                dump_ring();
                abort();
            }
            pack_flags();
            cpu->JumpTo(RET);
            continue;
        }
        stat_interp_steps++;
        if (pc < 0x01000000) {
            fprintf(stderr, "[recomp] PC went to %08X (lr=%08X sp=%08X cpsr=%08X)\n", pc, cpu->R[14], cpu->R[13], cpu->CPSR);
            dump_ring();
            abort();
        }
        if (trace && pc >= 0x02000000 && pc < 0x02400000) fallback_hist[pc]++;
        uint32_t oldmode = cpu->CPSR & 0x1F;
        cpu->StepOne();
        uint32_t newmode = cpu->CPSR & 0x1F;
        if (oldmode != newmode) {
            uint32_t npc = cur_pc();
            if (thread_mode(oldmode) && exc_mode(newmode) &&
                (npc == cpu->ExceptionBase + 0x18 || npc == cpu->ExceptionBase + 0x08)) {
                // exception entry (IRQ or SWI): remember where this thread resumes
                if (newmode == 0x12) cur->keys.push_back(Key(cpu->R[14] - 4, cpu->R_IRQ[0]));
                else                 cur->keys.push_back(Key(cpu->R[14], cpu->R_SVC[0]));
            } else if (exc_mode(oldmode) && thread_mode(newmode) && is_exc_return(cpu->CurInstr)) {
                if (on_exc_return() && has_stop) {
                    if (!(cur_pc() == stop && cpu->R[13] == stop_sp)) return true;
                }
            }
        }
        if (nds->ARM9Timestamp >= nds->ARM9Target) switch_to_emu();
    }
}

// Run code at `target` as a call returning to R[14]; sets RET.
static void call_via_interp(uint32_t target) {
    stat_fallbacks++;
    if (recomp_post_reset_log > 0) { recomp_post_reset_log--; fprintf(stderr, "[post-reset] interp call %08X lr=%08X\n", target, R[14]); }
    if (trace) { fprintf(stderr, "[recomp] interp call %08X lr=%08X\n", target, R[14]); }
    uint32_t lr = R[14], sp = R[13];
    pack_flags();
    cpu->JumpTo(target);
    bool diverted = run_interp(lr & ~1u, sp, true);
    unpack_flags();
    RET = diverted ? (cur_pc() | ((cpu->CPSR >> 5) & 1)) : lr;
}

// A native frame's callee came back somewhere unexpected (RET). Continue from there
// until this frame's own return point, then return normally.
extern "C" void divert(uint32_t entry_lr, uint32_t entry_sp) {
    stat_diverts++;
    if (trace) { fprintf(stderr, "[recomp] divert to %08X (frame returns to %08X)\n", RET, entry_lr); dump_ring(); }
    pack_flags();
    cpu->JumpTo(RET);
    bool diverted = run_interp(entry_lr & ~1u, entry_sp, true);
    unpack_flags();
    RET = diverted ? (cur_pc() | ((cpu->CPSR >> 5) & 1)) : entry_lr;
}

static void take_irq() {
    if (recomp_post_reset_log > 0) { recomp_post_reset_log--; fprintf(stderr, "[post-reset] irq IF=%08X IE=%08X\n", nds->IF[0], nds->IE[0]); }
    if (trace) fprintf(stderr, "[recomp] irq IF=%08X IE=%08X sp=%08X cpsr=%08X\n", nds->IF[0], nds->IE[0], cpu->R[13], cpu->CPSR);
    pack_flags();
    cpu->CPSR &= ~0x20u;
    cpu->R[15] = MAGIC + 4;
    uint32_t sp = cpu->R[13];
    cur->keys.push_back(Key(MAGIC, sp));
    cpu->TriggerIRQ();                 // LR_irq = MAGIC+4; BIOS returns via subs pc, lr, #4
    run_interp(MAGIC, sp, true);
    unpack_flags();
}

int recomp_inject_reset = 0;
extern "C" void native_check(void) {
    if (recomp_inject_reset == 1 && !(cpu->CPSR & 0x80) && (cpu->CPSR & 0x1F) == 0x1F) {
        recomp_inject_reset = 2;
        fprintf(stderr, "[recomp] test: calling OS_ResetSystem\n");
        R[14] = 0x02000000;
        call_addr(0x0207B930);
    }
    if (*ARM9TS >= *ARM9TG) switch_to_emu();
    if (cpu->IRQ && !(cpu->CPSR & 0x80)) take_irq();
}

extern "C" void recomp_saved_context(uint32_t ctx) {
    // OS_SaveContext stored a resume point (0x02079D4C, saved sp); a later OS_LoadContext
    // of this context comes back there via an exception return.
    Key k(SAVECTX_RESUME, rd32(ctx + 4 + 13 * 4));
    while (!cur->keys.empty() && cur->keys.back().first == SAVECTX_RESUME) cur->keys.pop_back();
    cur->keys.push_back(k);
}

// ---------------------------------------------------------------- calls / jumps from native code
extern "C" void call_addr(uint32_t addr) {
    if (is_reset_target(addr)) { pack_flags(); cpu->JumpTo(addr); if ((addr & ~1u) == FREEBIOS_ARM9_RESET) wait_for_arm7_reboot(); reset_world(); }
    if (const NativeFunc *nf = lookup(addr & ~1u)) { stat_native_calls++; stack_probe(); nf->fn(); return; }
    call_via_interp(addr);
}
extern "C" void call_indirect(uint32_t addr) { call_addr(addr); }
extern "C" void recomp_interp_call(uint32_t addr) { call_via_interp(addr); }
extern "C" int recomp_overlay_loaded(int ov) { return overlay_loaded(ov); }
extern "C" void recomp_icache_flush(void) { cpu->ICacheInvalidateAll(); }
extern "C" int recomp_cpu_ready(void) { return cpu != nullptr; }
extern "C" void indirect_jump(uint32_t addr) {
    if (is_reset_target(addr)) { pack_flags(); cpu->JumpTo(addr); if ((addr & ~1u) == FREEBIOS_ARM9_RESET) wait_for_arm7_reboot(); reset_world(); }
    // tail jump: target returns straight to our caller (R[14])
    if (const NativeFunc *nf = lookup(addr & ~1u)) { nf->fn(); return; }
    call_via_interp(addr);
}

extern "C" void swi(uint32_t n) {
    { uint32_t *h = swi_hist[swi_hist_pos++ & 15]; h[0] = n; h[1] = R[14]; h[2] = R[13]; }
    if (recomp_post_reset_log > 0) { recomp_post_reset_log--; fprintf(stderr, "[post-reset] swi %X r0=%08X r1=%08X r2=%08X\n", n, R[0], R[1], R[2]); }
    if (trace) fprintf(stderr, "[recomp] swi %X r0=%08X r1=%08X r2=%08X sp=%08X cpsr=%08X\n", n, R[0], R[1], R[2], R[13], cpu->CPSR);
    pack_flags();
    uint32_t sp = cpu->R[13];
    cur->keys.push_back(Key(MAGIC, sp));
    uint32_t old = cpu->CPSR & ~0x20u;
    cpu->CPSR = (old & ~0xBFu) | 0x93u;
    cpu->UpdateMode(old, cpu->CPSR);
    cpu->R_SVC[2] = old;
    wr32(MAGIC - 4, 0xEF000000u | (n << 16));   // BIOS reads the SWI number from [lr-4]
    cpu->R[14] = MAGIC;
    cpu->JumpTo(cpu->ExceptionBase + 0x08);
    run_interp(MAGIC, sp, true);
    unpack_flags();
}

// ---------------------------------------------------------------- memory
extern "C" uint32_t rd8(uint32_t a) { u32 v; cpu->DataRead8(a, &v); return v & 0xFF; }
extern "C" uint32_t rd16(uint32_t a) { u32 v; cpu->DataRead16(a & ~1u, &v); return v & 0xFFFF; }
extern "C" uint32_t rd32(uint32_t a) { u32 v; cpu->DataRead32(a & ~3u, &v); return v; }
static uint32_t watch_lo = 1, watch_hi = 0; static int watch_n = 0;
static inline void watch(uint32_t a, uint32_t v) {
    if (a >= watch_lo && a < watch_hi && watch_n < 400) {
        watch_n++;
        fprintf(stderr, "W %08X=%08X lr=%08X ring:", a, v, R[14]);
        for (unsigned i = 0; i < 8; i++) fprintf(stderr, " %08X", fn_ring[(fn_ring_pos - 8 + i) & 255]);
        fprintf(stderr, "\n");
    }
}
extern "C" void recomp_set_watch(uint32_t lo, uint32_t hi) { watch_lo = lo; watch_hi = hi; }
extern "C" void wr8(uint32_t a, uint32_t v) { watch(a, v); cpu->DataWrite8(a, v & 0xFF); }
extern "C" void wr16(uint32_t a, uint32_t v) { watch(a, v); cpu->DataWrite16(a & ~1u, v & 0xFFFF); }
extern "C" void wr32(uint32_t a, uint32_t v) { watch(a, v); cpu->DataWrite32(a & ~3u, v); }

// ---------------------------------------------------------------- system ops
static u32 *spsr_ptr() {
    switch (cpu->CPSR & 0x1F) {
        case 0x11: return &cpu->R_FIQ[7];
        case 0x12: return &cpu->R_IRQ[2];
        case 0x13: return &cpu->R_SVC[2];
        case 0x17: return &cpu->R_ABT[2];
        case 0x1B: return &cpu->R_UND[2];
    }
    return nullptr;
}
extern "C" uint32_t read_psr(int spsr) {
    pack_flags();
    if (spsr) { u32 *p = spsr_ptr(); return p ? *p : cpu->CPSR; }
    return cpu->CPSR;
}
extern "C" void write_psr(int spsr, int mask, uint32_t v) {
    uint32_t m = 0;
    if (mask & 1) m |= 0xFF;
    if (mask & 2) m |= 0xFF00;
    if (mask & 4) m |= 0xFF0000;
    if (mask & 8) m |= 0xFF000000;
    if (spsr) { u32 *p = spsr_ptr(); if (p) *p = (*p & ~m) | (v & m); return; }
    pack_flags();
    if ((cpu->CPSR & 0x1F) == 0x10) m &= 0xFF000000;
    uint32_t old = cpu->CPSR, nw = (old & ~m) | (v & m);
    cpu->CPSR = nw;
    if ((old & 0x1F) != (nw & 0x1F)) cpu->UpdateMode(old, nw);
    unpack_flags();
}
extern "C" void restore_spsr(void) { fprintf(stderr, "[recomp] restore_spsr from native code\n"); dump_ring(); }
extern "C" uint32_t mrc(int cp, int op1, int crn, int crm, int op2, int) {
    (void)op1;
    if (cp != 15) return 0;
    return cpu->CP15Read((crn << 8) | (crm << 4) | op2);
}
extern "C" void mcr(int cp, int op1, int crn, int crm, int op2, uint32_t v) {
    (void)op1;
    if (cp != 15) return;
    cpu->CP15Write((crn << 8) | (crm << 4) | op2, v);
    if (cpu->Halted) {                      // CP15 7,0,4 = wait for interrupt
        halt_safe_pc = recomp_halt_pc;      // while parked here the whole game is resumable from halt_pc+4
        while (cpu->Halted) handle_halt(true);
        halt_safe_pc = 0;
    }
}
extern "C" void bad_jump(uint32_t a) { fprintf(stderr, "[recomp] bad jump %08X\n", a); dump_ring(); abort(); }
extern "C" void unsupported_insn(uint32_t a, uint32_t w) { fprintf(stderr, "[recomp] unsupported %08X at %08X\n", w, a); abort(); }

static uint32_t sat(int64_t v) { return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? (uint32_t)INT32_MIN : (uint32_t)(int32_t)v; }
extern "C" uint32_t qadd(uint32_t a, uint32_t b) { return sat((int64_t)(int32_t)a + (int32_t)b); }
extern "C" uint32_t qsub(uint32_t a, uint32_t b) { return sat((int64_t)(int32_t)a - (int32_t)b); }
extern "C" uint32_t qdadd(uint32_t a, uint32_t b) { return sat((int64_t)(int32_t)a + (int32_t)sat(2 * (int64_t)(int32_t)b)); }
extern "C" uint32_t qdsub(uint32_t a, uint32_t b) { return sat((int64_t)(int32_t)a - (int32_t)sat(2 * (int64_t)(int32_t)b)); }
extern "C" uint32_t smla_q(uint32_t prod, uint32_t acc) { return prod + acc; }

// ---------------------------------------------------------------- entry
static void fiber_main() {
    // free fibers left behind by a soft reset (we're on a fresh stack, they never run again)
    if (!graveyard.empty()) { fprintf(stderr, "[recomp] fresh fiber running after reset (%zu old fibers retired)\n", graveyard.size()); recomp_post_reset_log = 6000; }
#ifndef _WIN32
    for (Fiber *g : graveyard) {
        if (g == cur) continue;
        free(g->stack);
        delete g;
    }
#endif
    graveyard.clear();              // on Windows the old fibers are simply left alone (small, rare)
    for (;;) run_interp(0xFFFFFFFF, 0, false);
}

static void hook(ARMv5 *c) {
    static bool started = false;
    if (!started) {
        started = true;
        cpu = c;
        R = c->R;
        ARM9TS = &nds->ARM9Timestamp;
        ARM9TG = &nds->ARM9Target;
        IRQP = &c->IRQ;
        for (unsigned i = 0; i < NATIVE_FUNC_COUNT; i++)
            table[NATIVE_FUNCS[i].addr].push_back(&NATIVE_FUNCS[i]);
#ifdef _WIN32
        emu_f.h = ConvertThreadToFiber(nullptr);
#endif
        cur = new_fiber();
    }
    switch_to_native();
}

// ---------------------------------------------------------------- quicksave / quickload
// A quicksave is a melonDS savestate taken while the game sits in its idle wait-for-interrupt. Native code keeps all of
// its state in the emulated registers and memory, so pointing PC just past that halt makes the state resumable by the
// interpreter, which hands calls back to native code as usual. Loading one retires every fiber (their C stacks belong
// to the old timeline) and starts a fresh one at the loaded CPU state.
extern "C" int recomp_quicksave_prepare(void) {
    if (!cpu || !halt_safe_pc || !cpu->Halted) return 0;
    pack_flags();
    cpu->JumpTo(halt_safe_pc + 4);
    cpu->Halted = 1;
    return 1;
}
extern "C" void recomp_quickload_reset(void) {
    if (!cpu) return;
    for (auto &kv : waiting) graveyard.push_back(kv.second);
    waiting.clear();
    if (cur) graveyard.push_back(cur);
#ifdef _WIN32
    for (Fiber *g : graveyard) { if (g->h) DeleteFiber(g->h); delete g; }
    graveyard.clear();
#endif
    halt_safe_pc = 0;
    cur = new_fiber();
    unpack_flags();
    fprintf(stderr, "[recomp] quickload: resuming at %08X (cpsr %08X)\n", cpu->R[15], cpu->CPSR);
}

void recomp_install(NDS *n) {
    nds = n;
    NativeExecuteHook = hook;
}

void recomp_arm7_state(FILE *f) {
    if (!nds) return;
    fprintf(f, "ARM7 pc=%08X cpsr=%08X halted=%d | IPCSYNC9=%04X IPCSYNC7=%04X | ARM7 IME=%d IE=%08X IF=%08X\n",
            nds->ARM7.R[15], nds->ARM7.CPSR, nds->ARM7.Halted, (unsigned)nds->ARM9Read16(0x04000180), (unsigned)nds->ARM7Read16(0x04000180), nds->IME[1], nds->IE[1], nds->IF[1]);
}

void recomp_stats(FILE *f) {
    recomp_arm7_state(f);
    fprintf(f, "ring:");
    for (unsigned i = 0; i < 24; i++) fprintf(f, " %08X", fn_ring[(fn_ring_pos - 24 + i) & 255]);
    fprintf(f, "\nARM9 R15=%08X CPSR=%08X Halted=%d IME=%d IE=%08X IF=%08X\n", cpu ? cpu->R[15] : 0, cpu ? cpu->CPSR : 0, cpu ? cpu->Halted : 0, nds ? nds->IME[0] : 0, nds ? nds->IE[0] : 0, nds ? nds->IF[0] : 0);
    fprintf(f, "native calls %llu, interpreted steps %llu, fallbacks %llu, diverts %llu, fibers %llu, thread switches %llu, parked %zu, resets %llu\n",
            (unsigned long long)stat_native_calls, (unsigned long long)stat_interp_steps, (unsigned long long)stat_fallbacks,
            (unsigned long long)stat_diverts, (unsigned long long)stat_fibers, (unsigned long long)stat_switches, waiting.size(), (unsigned long long)stat_resets);
    fprintf(f, "host stack: deepest %zu KB on a game fiber since last report\n", stat_stack_max >> 10); stat_stack_max = 0;
    if (trace) {
        std::vector<std::pair<uint64_t, uint32_t>> v;
        for (auto &kv : fallback_hist) v.push_back({kv.second, kv.first});
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < v.size() && i < 25; i++) fprintf(f, "  interp %08X x%llu\n", v[i].second, (unsigned long long)v[i].first);
    }
}
