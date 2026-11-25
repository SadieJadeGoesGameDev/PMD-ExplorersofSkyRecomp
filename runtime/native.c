/* Native replacements for SDK helpers whose ARM code is not worth recompiling. */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>

/* _s32_div_f / __divsi3: r0 = r0 / r1, r1 = r0 % r1 (signed) */
void f_arm9_208FEA4(void) {
    int32_t n = (int32_t)R[0], d = (int32_t)R[1];
    RET = R[14];
    if (d == 0) { R[1] = (uint32_t)n; R[0] = 0; return; }
    if (n == INT32_MIN && d == -1) { R[0] = (uint32_t)n; R[1] = 0; return; }
    R[0] = (uint32_t)(n / d);
    R[1] = (uint32_t)(n % d);
}

/* _u32_div_f / __udivsi3 */
void f_arm9_20900B0(void) {
    uint32_t n = R[0], d = R[1];
    RET = R[14];
    if (d == 0) { R[1] = n; R[0] = 0; return; }
    R[0] = n / d;
    R[1] = n % d;
}

/* _u32_div_not_0_f */
void f_arm9_20900B8(void) { f_arm9_20900B0(); }

/* OS_SaveContext: run the original, then tell the runtime this thread can be resumed later. */
void f_arm9_2079D08_orig(void);
void recomp_saved_context(uint32_t ctx);
void f_arm9_2079D08(void) {
    uint32_t ctx = R[0];
    f_arm9_2079D08_orig();
    recomp_saved_context(ctx);
}

/* ---- 64-bit helpers (r0:r1 = a, r2:r3 = b, result in r0:r1). Divide by zero returns a unchanged, like the SDK. */
static inline uint64_t a64(void) { return (uint64_t)R[0] | ((uint64_t)R[1] << 32); }
static inline uint64_t b64(void) { return (uint64_t)R[2] | ((uint64_t)R[3] << 32); }
static inline void ret64(uint64_t v) { R[0] = (uint32_t)v; R[1] = (uint32_t)(v >> 32); RET = R[14]; }

/* _ll_mod: signed remainder (C semantics: sign follows dividend) */
void f_arm9_208FC7C(void) {
    int64_t a = (int64_t)a64(), b = (int64_t)b64();
    if (b == 0) { RET = R[14]; return; }
    if (b == -1) { ret64(0); return; }
    ret64((uint64_t)(a % b));
}
/* _ll_sdiv */
void f_arm9_208FC8C(void) {
    int64_t a = (int64_t)a64(), b = (int64_t)b64();
    if (b == 0) { RET = R[14]; return; }
    if (b == -1) { ret64((uint64_t)0 - (uint64_t)a); return; }
    ret64((uint64_t)(a / b));
}
/* _ll_udiv */
void f_arm9_208FE3C(void) {
    uint64_t a = a64(), b = b64();
    if (b == 0) { RET = R[14]; return; }
    ret64(a / b);
}
/* _ull_mod */
void f_arm9_208FE48(void) {
    uint64_t a = a64(), b = b64();
    if (b == 0) { RET = R[14]; return; }
    ret64(a % b);
}
/* _ll_mul */
void f_arm9_208FE84(void) { ret64(a64() * b64()); }

/* ---- Widescreen: the dungeon BG is a 64x32 ring buffer but the game only fills the 33 columns under the
   256px view. Draw 6 extra columns (48px) on each side so the widened view has real map in its margins. */
#define WIDE_PX 48
extern void f_overlay29_2336F4C_orig(void);
extern void f_overlay29_233711C_orig(void);
extern void f_overlay29_23372A4_orig(void);
extern uint32_t rd32(uint32_t); extern uint32_t rd16(uint32_t); extern uint32_t rd8(uint32_t); extern void wr16(uint32_t, uint32_t); extern void wr32(uint32_t, uint32_t);
void f_overlay29_2336F4C(void) {              /* full redraw: 33 cols from camX>>3 -> run at camX-48, camX+48, camX */
    uint32_t lr = R[14], a = rd32(0x02353538) + 0x1A224, x = rd16(a);
    wr16(a, x - WIDE_PX); f_overlay29_2336F4C_orig();
    wr16(a, x + WIDE_PX); R[14] = lr; f_overlay29_2336F4C_orig();
    wr16(a, x);           R[14] = lr; f_overlay29_2336F4C_orig();
    RET = lr;
}
void f_overlay29_233711C(void) {              /* edge column: offset 0 (left) or 255 (right) */
    int32_t x = (int32_t)R[0];
    R[0] = (uint32_t)(x < 128 ? x - WIDE_PX : x + WIDE_PX);
    f_overlay29_233711C_orig();
}
void f_overlay29_23372A4(void) {              /* edge row: 33 cols from (camX+r0)>>3 -> twice, shifted */
    uint32_t lr = R[14], x = R[0], y = R[1];
    R[0] = x - WIDE_PX; f_overlay29_23372A4_orig();
    R[0] = x + WIDE_PX; R[1] = y; R[14] = lr; f_overlay29_23372A4_orig();
    RET = lr;
}

/* Sprite renderer's horizontal cull: vanilla hides sprites with x < -64 or x >= 256. Widen by WIDE_PX each side. */
void f_arm9_201B9B0(void) {
    int32_t x = (int32_t)R[0];
    R[0] = (x < -64 - WIDE_PX || x >= 256 + WIDE_PX) ? 1 : 0;
    RET = R[14];
}

/* Overworld (ground mode) BG refresh. The map is built from 3x3-tile chunks; vanilla copies a 12x9-chunk window
   (cols 2..37 of the 64-wide BG) whenever the camera crosses a chunk. Widescreen: also copy 2 chunks to the left and
   2 more to the right (16 chunks = 48 columns), blank outside the map (or wrapped, for looping maps).
   r0 = map_render struct. Four vanilla variants: 1 or 2 layers, clamped or wrapping. */
int recomp_wide_margin = 0;
uint32_t dbg_ground_o;
static void ground_refresh(int layers, int wrap) {
    uint32_t o = R[0]; dbg_ground_o = o;
    int32_t cx = (int32_t)rd32(o + 0x44), cy = (int32_t)rd32(o + 0x48);
    int32_t w = (int32_t)rd32(o + 0x8), h = (int32_t)rd32(o + 0xC);
    int extra = 2;
    uint32_t src[2] = {rd32(o + 0x24), rd32(o + 0x28)}, gfx[2] = {rd32(o + 0x1C), rd32(o + 0x20)}, dst[2] = {rd32(o + 0x2C), rd32(o + 0x30)};
    for (int strip = 0; strip < 9; strip++) {
        int32_t y = cy + strip;
        if (wrap && h > 0) y = ((y % h) + h) % h;
        for (int k = -extra; k < 12 + extra; k++) {
            int32_t x = cx + k;
            int inb = 1;
            if (wrap && w > 0) x = ((x % w) + w) % w;
            else if (k < 0 || k >= 12) inb = x >= 0 && x < w;
            for (int l = 0; l < layers; l++) {
                uint32_t idx = inb ? rd16(src[l] + (uint32_t)((y * 64 + x) * 2)) : 0;
                for (int sub = 0; sub < 3; sub++)
                    for (int t = 0; t < 3; t++) {
                        uint32_t v = inb ? rd16(gfx[l] + idx * 18 + (uint32_t)(sub * 3 + t) * 2) : 0;
                        int col = (2 + 3 * k + t) & 63, row = 3 * strip + sub;
                        wr16(dst[l] + (uint32_t)((col >> 5) * 0x800 + row * 64 + (col & 31) * 2), v);
                    }
            }
        }
    }
    TICK(1000 * layers);
    RET = R[14];
}
void f_overlay11_22EEAAC(void) { ground_refresh(1, 0); }
void f_overlay11_22EEBF8(void) { ground_refresh(1, 1); }
void f_overlay11_22EED98(void) { ground_refresh(2, 0); }
void f_overlay11_22EEFC0(void) { ground_refresh(2, 1); }

/* Ground camera, widescreen. ov11 0x22F1F3C(min*, max*) returns the range the camera *centre* may move in
   (map bounds +-128px x / +-96px y, 8.8 fixed point); sprites and the BG both follow that centre. Shrink the x range by
   the margin so the whole wide view stays on the map; maps narrower than the wide view get centred. */
extern void f_overlay11_22F1F3C_orig(void);
void f_overlay11_22F1F3C(void) {
    uint32_t lr = R[14], pmin = R[0], pmax = R[1];
    f_overlay11_22F1F3C_orig();
    int W = recomp_wide_margin;
    if (R[0] && W > 0) {
        int32_t lo = (int32_t)rd32(pmin), hi = (int32_t)rd32(pmax);
        int32_t mid = lo + (hi - lo) / 2;
        lo += W << 8; hi -= W << 8;
        if (lo > hi) lo = hi = mid;
        wr32(pmin, (uint32_t)lo); wr32(pmax, (uint32_t)hi);
    }
    RET = lr;
}

#include "settings.h"
RecompSettings recomp_settings = {1, FILTER_OFF, DISPLAY_WINDOWED, 0, 1, {1, 1, 1, 1, 1, 1, 1}, 0, 0, 0, 1};

/* =====================================================================================================
   Recomp Options: a submenu added to the game's own main menu, built from the game's own windows.
   Strings use private string IDs (0x7F00+) served by a hook on GetStringFromFile; menu item tables, extra-info
   blocks and option states live in unused ITCM (0x01FFC000..0x01FFE000).
   ===================================================================================================== */
#define SCR        0x01FFC000u
#define SCR_TOPITEMS (SCR + 0x000)   /* injected copy of the top menu's item table (<= 12 items) */
#define SCR_ITEMS    (SCR + 0x080)   /* our current menu's items */
#define SCR_CHOICES  (SCR + 0x200)   /* u16 choice lists */
#define SCR_EXTRA    (SCR + 0x400)   /* window_extra_info (0x98) */
#define SCR_STATES   (SCR + 0x500)   /* int option states in */
#define SCR_STATES2  (SCR + 0x540)   /* int option states out */
#define GCALL_RET    0x01FFFF40u
#define RM_RESULT    9               /* result value of our item in the top menu */

enum {
    S_RECOMP = 0x7F00, S_GRAPHICS, S_SADIE, S_SLOT, S_EXIT, S_WIDE, S_FILTER, S_DISPLAY, S_RES,
    S_OFF, S_ON, S_XBRZ, S_CRT, S_SMOOTH, S_GLOW, S_LCD, S_WINDOWED, S_BORDERLESS, S_FULLSCREEN,
    S_R0, S_R1, S_R2, S_R3, S_R4,
    S_SAD0, S_SAD1, S_SAD2, S_SAD3, S_SAD4, S_SAD5, S_SAD6,
    S_SLOT1, S_SLOT2, S_SLOT3, S_YES, S_NO, S_EXITQ, S_SLOTQ, S_AUTOSAVE, S_END
};
static const char *rm_strings[S_END - S_RECOMP] = {
    "Recomp Options", "Graphics", "Enhancements", "Save Slot", "Exit to Windows", "Widescreen", "Filter", "Display", "Resolution",
    "Off", "On", "xBRZ", "Scanlines", "Smooth", "Glow", "DS Screen", "Windowed", "Borderless", "Fullscreen",
    "1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160",
    "Pick Any Starter", "Full Team Control", "Gentler Hunger", "Keep Items on Defeat", "IQ Grows with Level", "Team EXP Share", "Recruit from Anyone",
    "Slot 1", "Slot 2", "Slot 3", "Yes", "No", "Exit to Windows?", "Switch slot? (restarts)", "Autosave"
};

/* GetStringFromFile(buffer, id): serve our private IDs */
extern void f_arm9_2025788_orig(void);
void f_arm9_2025788(void) {
    uint32_t id = R[1] & 0xFFFF;
    const char *sadie_string(uint32_t id);
    const char *ss = sadie_string(id);
    if ((id >= S_RECOMP && id < S_END) || ss) {
        const char *s = ss ? ss : rm_strings[id - S_RECOMP];
        uint32_t d = R[0];
        do wr8(d++, (uint8_t)*s); while (*s++);
        RET = R[14];
        return;
    }
    f_arm9_2025788_orig();
}

/* call a game function from native code; returns r0 */
static uint32_t gcall(uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, int ns, uint32_t s0, uint32_t s1) {
    uint32_t sv[16];
    for (int i = 4; i < 16; i++) sv[i] = R[i];
    uint32_t sp = (R[13] - 16) & ~7u;
    if (ns > 0) wr32(sp, s0);
    if (ns > 1) wr32(sp + 4, s1);
    R[13] = sp; R[0] = a0; R[1] = a1; R[2] = a2; R[3] = a3; R[14] = GCALL_RET;
    call_addr(addr);
    uint32_t r = R[0];
    for (int i = 4; i < 16; i++) if (i != 15) R[i] = sv[i];
    return r;
}
static void wzero(uint32_t a, uint32_t n) { for (uint32_t i = 0; i < n; i += 4) wr32(a + i, 0); }

/* ---- top ground menu: inject our item */
static const uint32_t TOP_TABLES[4] = {0x23225FC, 0x232262C, 0x2322654, 0x2322684};
extern void f_arm9_202A5CC_orig(void);
void f_arm9_202A5CC(void) {
    uint32_t items = R[3];
    int is_top = 0;
    for (int i = 0; i < 4; i++) if (items == TOP_TABLES[i]) is_top = 1;
    if (items == 0x2389DD4) is_top = 2;                    /* dungeon main menu */
    if (is_top) {
        int n = 0;
        while (n < 10 && (rd32(items + n * 8) & 0xFFFF)) { wr32(SCR_TOPITEMS + n * 8, rd32(items + n * 8)); wr32(SCR_TOPITEMS + n * 8 + 4, rd32(items + n * 8 + 4)); n++; }
        wr32(SCR_TOPITEMS + (n + 1) * 8, rd32(items + n * 8)); wr32(SCR_TOPITEMS + (n + 1) * 8 + 4, rd32(items + n * 8 + 4));
        int at = n;                                       /* put "Recomp Options" just above "Exit" */
        if (n > 0 && rd32(items + (n - 1) * 8 + 4) == (is_top == 2 ? 0xFFFFFFFFu : 7u)) {
            at = n - 1;
            wr32(SCR_TOPITEMS + n * 8, rd32(items + at * 8)); wr32(SCR_TOPITEMS + n * 8 + 4, rd32(items + at * 8 + 4));
        }
        wr32(SCR_TOPITEMS + at * 8, S_RECOMP); wr32(SCR_TOPITEMS + at * 8 + 4, RM_RESULT);
        uint32_t st = rd32(is_top == 2 ? 0x238A260 : 0x2324D34);
        if (st) { wr8(st + 8 + n, rd8(st + 8 + at)); wr8(st + 8 + at, 0); }   /* per-item enable flags follow the items */
        R[3] = SCR_TOPITEMS;
        if (is_top == 2 && R[0]) {                        /* dungeon menu has a fixed height: one row taller */
            for (int i = 0; i < 16; i += 4) wr32(SCR + 0x600 + i, rd32(R[0] + i));
            uint32_t y = rd8(SCR + 0x605), h = rd8(SCR + 0x607);
            if (h) { wr8(SCR + 0x607, h + 2); if (y > 1) wr8(SCR + 0x605, y - 1); }
            R[0] = SCR + 0x600;
        }
    }
    f_arm9_202A5CC_orig();
}

/* ---- our menu state machine */
enum { RM_IDLE, RM_MAIN, RM_GFX, RM_SADIE, RM_SLOT, RM_SLOTQ, RM_EXITQ, RM_AUTO, RM_DONE };
enum { W_PARENT, W_OPTIONS, W_SIMPLE };
static int rm_state = RM_IDLE, rm_win = -2, rm_kind, rm_phase, rm_n, rm_pick_slot, rm_main_choice = 0;
static int rm_result, rm_confirmed;

static void rm_extra(uint32_t title) {
    wzero(SCR_EXTRA, 0x98);
    if (title) { wr16(SCR_EXTRA + 8, title); wr32(SCR_EXTRA + 0xC, 16); }
}
static void rm_items(const uint16_t *ids, const int *res, int n) {   /* simple_menu_id_item list */
    for (int i = 0; i < n; i++) { wr32(SCR_ITEMS + i * 8, ids[i]); wr32(SCR_ITEMS + i * 8 + 4, (uint32_t)res[i]); }
    wr32(SCR_ITEMS + n * 8, 0); wr32(SCR_ITEMS + n * 8 + 4, 1);
}
static void rm_open_parent(void) {
    static const uint16_t ids[5] = {S_GRAPHICS, S_SADIE, S_AUTOSAVE, S_SLOT, S_EXIT};
    static const int res[5] = {2, 3, 6, 4, 5};
    rm_items(ids, res, 5);
    rm_extra(S_RECOMP);
    wr32(SCR_EXTRA + 0, (uint32_t)rm_main_choice);
    rm_win = (int8_t)gcall(0x202A5CC, 0, 0x13 | 0x20 | 0x800, SCR_EXTRA, SCR_ITEMS, 0, 0, 0);
    rm_kind = W_PARENT; rm_phase = 0;
    fprintf(stderr, "[rm] parent window %d\n", rm_win);
}
static void rm_open_simple(uint32_t title, const uint16_t *ids, const int *res, int n) {
    rm_items(ids, res, n);
    rm_extra(title);
    rm_win = (int8_t)gcall(0x202B0EC, 0, 0x13 | 0x800, SCR_EXTRA, SCR_ITEMS, 1, (uint32_t)n, 0);
    rm_kind = W_SIMPLE; rm_phase = 0;
}
/* options menu: items[i] has a choice list; states = current values */
static void rm_open_options(uint32_t title, const uint16_t *labels, const uint16_t *const *choices, const int *states, int n) {
    uint32_t ch = SCR_CHOICES;
    for (int i = 0; i < n; i++) {
        uint32_t list = ch;
        for (const uint16_t *c = choices[i]; ; c++) { wr16(ch, *c); ch += 2; if (!*c) break; }
        ch = (ch + 3) & ~3u;
        wr32(SCR_ITEMS + i * 12, labels[i]); wr32(SCR_ITEMS + i * 12 + 4, 0); wr32(SCR_ITEMS + i * 12 + 8, list);
        wr32(SCR_STATES + i * 4, (uint32_t)states[i]);
    }
    wr32(SCR_ITEMS + n * 12, 0); wr32(SCR_ITEMS + n * 12 + 4, 0); wr32(SCR_ITEMS + n * 12 + 8, 0);
    rm_extra(title);
    /* window params like the game's own Options screen: x=2, y=2 tiles, auto size */
    wr32(SCR + 0x610, 0); wr32(SCR + 0x614, 0x00000202); wr32(SCR + 0x618, 0x0000FF00); wr32(SCR + 0x61C, 0);
    rm_win = (int8_t)gcall(0x202D1F0, SCR + 0x610, 0x33 | 0x1800, SCR_EXTRA, SCR_ITEMS, 2, (uint32_t)n, SCR_STATES);
    rm_kind = W_OPTIONS; rm_phase = 0; rm_n = n;
}
/* poll the open window; returns 1 once it has been closed (rm_result / rm_confirmed set) */
static int rm_poll(void) {
    if (rm_win < 0) { rm_result = 1; rm_confirmed = 0; return 1; }
    if (rm_kind == W_PARENT) {
        if (rm_phase == 0) { if (gcall(0x202AB40, (uint32_t)rm_win, 0, 0, 0, 0, 0, 0)) return 0; rm_result = (int)gcall(0x202ABB0, (uint32_t)rm_win, 0, 0, 0, 0, 0, 0); rm_phase = 1; }
        gcall(0x202AABC, (uint32_t)rm_win, 0, 0, 0, 0, 0, 0);
    } else if (rm_kind == W_SIMPLE) {
        if (rm_phase == 0) { if (gcall(0x202B4F0, (uint32_t)rm_win, 0, 0, 0, 0, 0, 0)) return 0; rm_result = (int)gcall(0x202B57C, (uint32_t)rm_win, 0, 0, 0, 0, 0, 0); rm_phase = 1; }
        gcall(0x202B4C4, (uint32_t)rm_win, 0, 0, 0, 0, 0, 0);
    } else {
        if (rm_phase == 0) {
            if (gcall(0x202D57C, (uint32_t)rm_win, 0, 0, 0, 0, 0, 0)) return 0;
            rm_confirmed = (gcall(0x202D5E4, (uint32_t)rm_win, 0, 0, 0, 0, 0, 0) & 0xFF) != 0;
            if (rm_confirmed) gcall(0x202D5B0, (uint32_t)rm_win, SCR_STATES2, 0, 0, 0, 0, 0);
            rm_phase = 1;
        }
        if (gcall(0x202D55C, (uint32_t)rm_win, 0, 0, 0, 0, 0, 0)) return 0;
        gcall(0x202D530, (uint32_t)rm_win, 0, 0, 0, 0, 0, 0);
    }
    rm_win = -2;
    return 1;
}
static const uint16_t CH_ONOFF[] = {S_OFF, S_ON, 0}, CH_FILTER[] = {S_OFF, S_XBRZ, S_CRT, S_SMOOTH, S_GLOW, S_LCD, 0},
    CH_DISPLAY[] = {S_WINDOWED, S_BORDERLESS, S_FULLSCREEN, 0}, CH_RES[] = {S_R0, S_R1, S_R2, S_R3, S_R4, 0};
#include <stdio.h>
static void rm_enter(int st) {
    RecompSettings *S = &recomp_settings;
    rm_state = st;
    fprintf(stderr, "[rm] enter %d\n", st);
    if (st == RM_MAIN) rm_open_parent();
    else if (st == RM_GFX) {
        static const uint16_t lab[3] = {S_FILTER, S_DISPLAY, S_RES};          // widescreen is always on
        const uint16_t *ch[3] = {CH_FILTER, CH_DISPLAY, CH_RES};
        int stt[3] = {S->filter, S->display, S->resolution};
        rm_open_options(S_GRAPHICS, lab, ch, stt, 3);
    } else if (st == RM_SADIE) {
        static const uint16_t lab[7] = {S_SAD0, S_SAD1, S_SAD2, S_SAD3, S_SAD4, S_SAD5, S_SAD6};
        const uint16_t *ch[7] = {CH_ONOFF, CH_ONOFF, CH_ONOFF, CH_ONOFF, CH_ONOFF, CH_ONOFF, CH_ONOFF};
        rm_open_options(S_SADIE, lab, ch, S->sadie, 7);
    } else if (st == RM_AUTO) {
        static const uint16_t lab[1] = {S_AUTOSAVE};
        const uint16_t *ch[1] = {CH_ONOFF};
        int stt[1] = {S->autosave};
        rm_open_options(S_AUTOSAVE, lab, ch, stt, 1);
    } else if (st == RM_SLOT) {
        static const uint16_t ids[3] = {S_SLOT1, S_SLOT2, S_SLOT3};
        static const int res[3] = {2, 3, 4};
        rm_open_simple(S_SLOT, ids, res, 3);
    } else if (st == RM_SLOTQ || st == RM_EXITQ) {
        static const uint16_t ids[2] = {S_YES, S_NO};
        static const int res[2] = {2, 3};
        rm_open_simple(st == RM_EXITQ ? S_EXITQ : S_SLOTQ, ids, res, 2);
    }
}
/* one frame of our menus; returns 1 when the whole Recomp Options flow is finished */
static int rm_update(void) {
    RecompSettings *S = &recomp_settings;
    if (!rm_poll()) return 0;
    fprintf(stderr, "[rm] state %d closed: result %d confirmed %d\n", rm_state, rm_result, rm_confirmed);
    switch (rm_state) {
    case RM_MAIN:
        switch (rm_result) {
        case 2: rm_main_choice = 0; rm_enter(RM_GFX); return 0;
        case 3: rm_main_choice = 1; rm_enter(RM_SADIE); return 0;
        case 6: rm_main_choice = 2; rm_enter(RM_AUTO); return 0;
        case 4: rm_main_choice = 3; rm_enter(RM_SLOT); return 0;
        case 5: rm_main_choice = 4; rm_enter(RM_EXITQ); return 0;
        default: rm_state = RM_IDLE; return 1;
        }
    case RM_GFX:
        if (rm_confirmed) {
            S->filter = (int)rd32(SCR_STATES2 + 0) % FILTER_COUNT;
            S->display = (int)rd32(SCR_STATES2 + 4) % DISPLAY_COUNT;
            S->resolution = (int)rd32(SCR_STATES2 + 8) % 5;
            S->dirty = 1;
        }
        rm_enter(RM_MAIN); return 0;
    case RM_AUTO:
        if (rm_confirmed) { S->autosave = rd32(SCR_STATES2) != 0; S->dirty = 1; }
        rm_enter(RM_MAIN); return 0;
    case RM_SADIE:
        if (rm_confirmed) { for (int i = 0; i < SADIE_COUNT; i++) S->sadie[i] = rd32(SCR_STATES2 + i * 4) != 0; S->dirty = 1; }
        rm_enter(RM_MAIN); return 0;
    case RM_SLOT:
        if (rm_result >= 2 && rm_result <= 4 && rm_result - 1 != S->save_slot) { rm_pick_slot = rm_result - 1; rm_enter(RM_SLOTQ); return 0; }
        rm_enter(RM_MAIN); return 0;
    case RM_SLOTQ:
        if (rm_result == 2) { S->request_slot = rm_pick_slot; rm_state = RM_IDLE; return 1; }
        rm_enter(RM_MAIN); return 0;
    case RM_EXITQ:
        if (rm_result == 2) { S->request_exit = 1; rm_state = RM_IDLE; return 1; }
        rm_enter(RM_MAIN); return 0;
    }
    rm_state = RM_IDLE;
    return 1;
}

/* UpdateTopGroundMenu: run our menus in place of the top menu while they're open */
extern void f_overlay11_2300F50_orig(void);
void f_overlay11_2300F50(void) {
    uint32_t lr = R[14];
    uint32_t st = rd32(0x2324D34);
    if (rm_state != RM_IDLE) {
        if (!rm_update()) { R[0] = 1; RET = lr; return; }
        wr32(0x2324D3C, 1);                      /* finished: let the top menu close as if cancelled */
    } else if (st && rd32(st + 4) == 4 && rd32(0x2324D3C) == RM_RESULT) {
        rm_main_choice = 0;
        rm_enter(RM_MAIN);
        R[0] = 1; RET = lr; return;
    }
    R[14] = lr;
    f_overlay11_2300F50_orig();
}

/* DungeonMenuLoop (overlay 31): same idea for the dungeon main menu */
extern void f_overlay31_2382B54_orig(void);
void f_overlay31_2382B54(void) {
    uint32_t lr = R[14];
    uint32_t st = rd32(0x238A260);
    if (rm_state != RM_IDLE) {
        if (!rm_update()) { R[0] = 1; RET = lr; return; }
        /* finished: close the dungeon menu as if cancelled */
        wr32(0x238A264, 0xFFFFFFFFu);
        if (st) wr32(st + 4, rd32(st + 4) + 1);
        R[0] = 4; RET = lr; return;
    }
    if (st && rd32(st + 4) == 3 && (int)gcall(0x202ABB0, (uint32_t)(int8_t)rd8(st), 0, 0, 0, 0, 0, 0) == RM_RESULT) {
        rm_main_choice = 0;
        rm_enter(RM_MAIN);
        R[0] = 1; RET = lr; return;
    }
    R[14] = lr;
    f_overlay31_2382B54_orig();
}

/* =====================================================================================================
   Sadie Edition: patch groups applied to / removed from RAM live, following the toggles. Functions they touch run
   in the interpreter while their group is on (see sadie mask in the dispatch table), so the patched bytes run.
   ===================================================================================================== */
#include "sadie_patches.h"
extern int recomp_overlay_loaded(int ov);
extern void recomp_icache_flush(void);
static int sadie_run_state(const SadiePatch *p) {   /* 1 = vanilla in RAM, 2 = patched, 0 = something else */
    int v = 1, q = 1;
    for (uint32_t i = 0; i < p->len && (v || q); i++) {
        uint32_t b = rd8(p->addr + i);
        if (b != p->vanilla[i]) v = 0;
        if (b != p->patched[i]) q = 0;
    }
    return q ? 2 : v ? 1 : 0;
}
extern int recomp_cpu_ready(void);
/* RAM is only re-patched when an overlay loads (new floor / new area): never under code that's mid-run. Until then a
   switched-off group keeps its bytes but runs vanilla natively; a switched-on group runs the vanilla bytes interpreted. */
static void sadie_apply(void) {
    int changed = 0;
    for (int i = 0; i < SADIE_PATCH_COUNT; i++) {
        const SadiePatch *p = &SADIE_PATCHES[i];
        if (p->bin >= 0 && !recomp_overlay_loaded(p->bin)) continue;
        int want = recomp_settings.sadie[p->group] ? 2 : 1, have = sadie_run_state(p);
        if (have == 0 || have == want) continue;
        const uint8_t *src = want == 2 ? p->patched : p->vanilla;
        for (uint32_t k = 0; k < p->len; k++) wr8(p->addr + k, src[k]);
        changed = 1;
    }
    if (changed) recomp_icache_flush();
}
void sadie_sync(void) {            /* frontend, every frame: just the first patch-up after boot */
    static int done = 0;
    if (done || !recomp_cpu_ready()) return;
    done = 1;
    sadie_apply();
}
const char *sadie_string(uint32_t id) {
    for (int i = 0; i < SADIE_STRING_COUNT; i++)
        if ((uint32_t)SADIE_STRINGS[i].id == id && recomp_settings.sadie[SADIE_STRINGS[i].group]) return SADIE_STRINGS[i].text;
    return 0;
}
/* LoadOverlay(group): patch the freshly loaded overlay before any of its code runs */
extern void f_arm9_20040AC_orig(void);
void f_arm9_20040AC(void) {
    uint32_t lr = R[14];
    f_arm9_20040AC_orig();
    uint32_t r0 = R[0];
    sadie_apply();
    R[0] = r0;
    RET = lr;
}
int recomp_menu_busy(void) { return rm_state != RM_IDLE; }
void recomp_menu_reset(void) { rm_state = RM_IDLE; rm_win = -2; }

/* ---------------------------------------------------------------- portraits beside the text box (widescreen)
   Reads the game's window list. When a portrait box and a dialogue box are both showing on the same screen,
   returns where both are drawn and where they should go: portrait beside the text box at the same height, the pair
   centred across the wide screen. out: {n, screen, then per window sx, sy, sw, sh, dx, dy} in pixels. */
#define WINDOW_LIST 0x022A88DCu
#define UPD_PORTRAIT 0x0202F70Cu
#define UPD_DIALOGUE 0x0202F488u
void recomp_portrait_move(int *out) {
    out[0] = 0;
    if (!recomp_cpu_ready() || !recomp_wide_margin) return;
    int pw = -1, dw = -1;
    for (int i = 0; i < 20; i++) {
        uint32_t w = WINDOW_LIST + i * 224, upd = rd32(w);
        if (!upd || (int8_t)rd8(w + 0xB6) == 0) continue;
        if (upd == UPD_PORTRAIT && pw < 0) {
            uint32_t box = rd32(w + 0xC), st = box ? rd32(box + 4) : 0;
            if (rd8(box + 0x2A) && st >= 1 && st <= 3) pw = i;   /* framed portrait that is showing */
        } else if (upd == UPD_DIALOGUE && dw < 0) dw = i;
    }
    if (pw < 0 || dw < 0) return;
    uint32_t p = WINDOW_LIST + pw * 224, d = WINDOW_LIST + dw * 224;
    int scr = rd8(p + 8);
    if (scr != rd8(d + 8)) return;
    const int P = 6, GAP = 2;                          /* the frame sits ~5 px outside the window */
    int psx = rd8(p + 4) * 8 - P, psy = rd8(p + 5) * 8 - P, psw = rd8(p + 6) * 8 + 2 * P, psh = rd8(p + 7) * 8 + 2 * P;
    int tsx = rd8(d + 4) * 8 - P, tsy = rd8(d + 5) * 8 - P, tsw = rd8(d + 6) * 8 + 2 * P, tsh = rd8(d + 7) * 8 + 2 * P;
    /* centre portrait + gap + text box as one group across the wide screen */
    int left = (256 - (psw + GAP + tsw)) / 2;
    if (left < -recomp_wide_margin) left = -recomp_wide_margin;
    out[0] = 2; out[1] = scr;
    out[2] = psx; out[3] = psy; out[4] = psw; out[5] = psh; out[6] = left; out[7] = tsy;
    out[8] = tsx; out[9] = tsy; out[10] = tsw; out[11] = tsh; out[12] = left + psw + GAP; out[13] = tsy;
}
