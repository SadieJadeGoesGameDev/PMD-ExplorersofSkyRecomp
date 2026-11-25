// Sky Recomp — Pokémon Mystery Dungeon: Explorers of Sky (USA), native PC port.
// Recompiled ARM9 game code runs natively; melonDS provides the DS hardware.
// v0.1: two-screen window, keyboard + controller, sound, saves.
extern "C" int recomp_wide_margin;
#include <SDL.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <cstdio>
#include <csignal>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include "NDS.h"
#include "NDSCart.h"
#include "GPU_Soft.h"
#include "SPU.h"
#include "Args.h"
#include "Savestate.h"
#include "settings.h"
#include <cmath>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
namespace filters { void xbr4(const uint32_t *, int, int, uint32_t *); void crt(const uint32_t *, int, int, uint32_t *, int, int);
    void smooth(const uint32_t *, int, int, uint32_t *, int, int, int); void glow(const uint32_t *, int, int, uint32_t *, int, int); }

using namespace melonDS;
void recomp_install(NDS *);
extern "C" void sadie_sync(void);
extern "C" int recomp_quicksave_prepare(void);
extern "C" void recomp_quickload_reset(void);
extern "C" void recomp_portrait_move(int *);
extern "C" int recomp_menu_busy(void);
extern "C" void recomp_menu_reset(void);
extern "C" int recomp_overlay_loaded(int ov);
extern "C" int recomp_cpu_ready(void);
namespace melonDS::Platform { extern std::string g_save_path; extern volatile int g_save_writes; }

static const uint32_t VANILLA_CODE_CRC = 0xCFB1B178;   // arm9 + all overlays of the clean USA ROM

static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n) {
    static uint32_t t[256];
    if (!t[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t x = i; for (int k = 0; k < 8; k++) x = x & 1 ? 0xEDB88320u ^ (x >> 1) : x >> 1; t[i] = x; }
    c = ~c;
    while (n--) c = t[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}
static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static bool rom_is_vanilla(const uint8_t *rom, size_t len) {
    if (len < 0x200 || memcmp(rom + 0xC, "C2SE", 4) != 0) return false;
    uint32_t a9o = rd32le(rom + 0x20), a9s = rd32le(rom + 0x2C);
    uint32_t y9o = rd32le(rom + 0x50), y9s = rd32le(rom + 0x54), fat = rd32le(rom + 0x48);
    if ((size_t)a9o + a9s > len || (size_t)y9o + y9s > len) return false;
    uint32_t c = crc32_update(0, rom + a9o, a9s);
    for (uint32_t k = 0; k < y9s / 32; k++) {
        uint32_t fid = rd32le(rom + y9o + k * 32 + 24);
        uint32_t s = rd32le(rom + fat + fid * 8), e = rd32le(rom + fat + fid * 8 + 4);
        if (e > len || e < s) return false;
        c = crc32_update(c, rom + s, e - s);
    }
    return c == VANILLA_CODE_CRC;
}

static std::string exe_dir() {
    char *b = SDL_GetBasePath();
    std::string s = b ? b : "./";
    SDL_free(b);
    return s;
}

static std::vector<uint8_t> read_file(const std::string &p) {
    std::vector<uint8_t> v;
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return v;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    v.resize(n > 0 ? n : 0);
    if (n > 0 && fread(v.data(), 1, n, f) != (size_t)n) v.clear();
    fclose(f);
    return v;
}

static void fatal(const char *msg) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Sky Recomp", msg, nullptr);
    exit(1);
}

// DS key bits (active low in the mask): A B Select Start Right Left Up Down R L X Y
enum { K_A, K_B, K_SELECT, K_START, K_RIGHT, K_LEFT, K_UP, K_DOWN, K_R, K_L, K_X, K_Y };

extern "C" uint32_t fn_ring[256];
extern "C" unsigned fn_ring_pos;
extern "C" uint32_t *R;
void recomp_stats(FILE *);
static void rec_flush();
#ifdef _WIN32
static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
    rec_flush();
    fprintf(stderr, "\n*** CRASH: exception %08lX at %p", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    if (ep->ExceptionRecord->NumberParameters >= 2)
        fprintf(stderr, " (access %s %p)", ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read", (void *)ep->ExceptionRecord->ExceptionInformation[1]);
    fprintf(stderr, "\n");
    if (R) { fprintf(stderr, "ARM9 regs:"); for (int i = 0; i < 16; i++) fprintf(stderr, " r%d=%08X", i, R[i]); fprintf(stderr, "\n"); }
    fprintf(stderr, "last native functions:");
    for (unsigned i = 0; i < 64; i++) fprintf(stderr, "%s%08X", i % 8 ? " " : "\n ", fn_ring[(fn_ring_pos - 64 + i) & 255]);
    fprintf(stderr, "\n");
    recomp_stats(stderr);
    fflush(stderr);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Sky Recomp crashed",
        "Sky Recomp crashed. Details were written to recomp_log.txt next to SkyRecomp.exe - send that file to Claude.", nullptr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif
static void on_abort(int) {
    rec_flush();
    fprintf(stderr, "\n*** ABORT (see messages above)\n");
    fflush(stderr);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Sky Recomp stopped",
        "Sky Recomp hit an error it couldn't recover from. Details were written to recomp_log.txt next to SkyRecomp.exe - send that file to Claude.", nullptr);
    _exit(3);
}

static volatile uint64_t g_frames = 0;
static void watchdog_thread() {
    uint64_t last = 0; int stuck = 0, reports = 0;
    for (;;) {
        SDL_Delay(500);
        if (g_frames == last && g_frames > 0) {
            if (++stuck == 6 && reports < 5) {          // 3 seconds without a frame
                reports++;
                fprintf(stderr, "\n*** WATCHDOG: no frame for 3s (frame %llu)\n", (unsigned long long)g_frames);
                if (R) { fprintf(stderr, "ARM9 regs:"); for (int i = 0; i < 16; i++) fprintf(stderr, " r%d=%08X", i, R[i]); fprintf(stderr, "\n"); }
                fprintf(stderr, "last native functions:");
                for (unsigned i = 0; i < 64; i++) fprintf(stderr, "%s%08X", i % 8 ? " " : "\n ", fn_ring[(fn_ring_pos - 64 + i) & 255]);
                fprintf(stderr, "\n");
                recomp_stats(stderr);
            }
            if (stuck > 6 && stuck % 10 == 0 && reports < 5) {   // a few more snapshots while still stuck
                reports++;
                fprintf(stderr, "*** still stuck; ring now:");
                for (unsigned i = 0; i < 16; i++) fprintf(stderr, " %08X", fn_ring[(fn_ring_pos - 16 + i) & 255]);
                fprintf(stderr, "\n");
                recomp_stats(stderr);
            }
        } else { stuck = 0; last = g_frames; }
    }
}


// ---------------------------------------------------------------- settings (SkyRecomp.ini next to the exe)
static const char *SADIE_KEYS[SADIE_COUNT] = {"any_starter", "team_control", "gentle_hunger", "keep_items", "iq_per_level", "team_exp"};
struct Res { int w, h; };
static const Res RESOLUTIONS[] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
static const int NUM_RES = sizeof(RESOLUTIONS) / sizeof(RESOLUTIONS[0]);
static std::string g_ini_path;
static void settings_load() {
    RecompSettings &S = recomp_settings;
    FILE *f = fopen(g_ini_path.c_str(), "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char k[64]; int v;
        if (sscanf(line, " %63[^= ] = %d", k, &v) != 2) continue;
        std::string key = k;
        if (key == "filter") S.filter = v >= 0 && v < FILTER_COUNT ? v : 0;
        else if (key == "display") S.display = v >= 0 && v < DISPLAY_COUNT ? v : 0;
        else if (key == "resolution") S.resolution = v >= 0 && v < NUM_RES ? v : 0;
        else if (key == "save_slot") S.save_slot = v >= 1 && v <= 3 ? v : 1;
        else if (key == "autosave") S.autosave = v != 0;
        else for (int i = 0; i < SADIE_COUNT; i++) if (key == SADIE_KEYS[i]) S.sadie[i] = v != 0;
    }
    fclose(f);
}
static void settings_save() {
    RecompSettings &S = recomp_settings;
    FILE *f = fopen(g_ini_path.c_str(), "w");
    if (!f) return;
    fprintf(f, "; Sky Recomp settings (also changeable in-game: main menu > Recomp Options)\n");
    fprintf(f, "filter = %d   ; 0 off, 1 xBRZ, 2 scanlines, 3 smooth, 4 glow, 5 DS screen\ndisplay = %d   ; 0 windowed, 1 borderless fullscreen, 2 fullscreen\n", S.filter, S.display);
    fprintf(f, "resolution = %d   ; 0 1280x720, 1 1600x900, 2 1920x1080, 3 2560x1440, 4 3840x2160\nsave_slot = %d\n", S.resolution, S.save_slot);
    fprintf(f, "autosave = %d\n", S.autosave);
    fprintf(f, "; Enhancements\n");
    for (int i = 0; i < SADIE_COUNT; i++) fprintf(f, "%s = %d\n", SADIE_KEYS[i], S.sadie[i]);
    fclose(f);
}


// ---------------------------------------------------------------- save gear (placeholder art)
// A little clockwork gear that spins in the corner whenever the save file is written.
static SDL_Texture *make_gear_texture(SDL_Renderer *ren) {
    const int N = 128; std::vector<uint32_t> px(N * N);
    const float PI = 3.14159265f;
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
        float acc[4] = {0, 0, 0, 0};
        for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {       // 4x4 supersampling
            float fx = (x + (sx + 0.5f) / 4) / N * 2 - 1, fy = (y + (sy + 0.5f) / 4) / N * 2 - 1;
            float r = sqrtf(fx * fx + fy * fy), a = atan2f(fy, fx);
            float tooth = fmodf(a / (2 * PI) * 10 + 10.0f, 1.0f);                  // 10 teeth
            float outer = (tooth > 0.25f && tooth < 0.75f) ? 0.97f : 0.80f;
            float cr = 0, cg = 0, cb = 0, ca = 0;
            if (r < outer && r > 0.22f) {
                bool rim = r > outer - 0.09f || r < 0.31f;
                if (rim) { cr = 30; cg = 85; cb = 110; } else { cr = 80; cg = 190; cb = 210; }
                ca = 255;
                // clock face ring + hands, for the "time" in time gear
                if (r > 0.52f && r < 0.58f) { cr = 30; cg = 85; cb = 110; }
            }
            float hx = fx, hy = fy;
            if ((fabsf(hx) < 0.05f && hy < 0 && hy > -0.48f) || (fabsf(hy) < 0.05f && hx > 0 && hx < 0.36f)) { cr = 250; cg = 215; cb = 90; ca = 255; }
            if (r <= 0.22f) { cr = 250; cg = 215; cb = 90; ca = r < 0.12f ? 255 : 255; if (r > 0.16f) { cr = 30; cg = 85; cb = 110; } }
            acc[0] += cr * ca / 255; acc[1] += cg * ca / 255; acc[2] += cb * ca / 255; acc[3] += ca;
        }
        float A = acc[3] / 16;
        uint32_t R = A > 0 ? (uint32_t)(acc[0] / 16 * 255 / A) : 0, G = A > 0 ? (uint32_t)(acc[1] / 16 * 255 / A) : 0, B = A > 0 ? (uint32_t)(acc[2] / 16 * 255 / A) : 0;
        px[y * N + x] = (uint32_t)A << 24 | (R > 255 ? 255 : R) << 16 | (G > 255 ? 255 : G) << 8 | (B > 255 ? 255 : B);
    }
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, N, N);
    SDL_UpdateTexture(t, nullptr, px.data(), N * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    return t;
}
// t: seconds since the save; draws nothing after the animation ends
static void draw_gear(SDL_Renderer *ren, SDL_Texture *gear, double t) {
    const double G_IN = 0.25, G_HOLD = 2.5, G_OUT = 0.5;
    if (t < 0 || t > G_IN + G_HOLD + G_OUT) return;
    double alpha = t < G_IN ? t / G_IN : t < G_IN + G_HOLD ? 1.0 : 1.0 - (t - G_IN - G_HOLD) / G_OUT;
    int lw, lh; SDL_RenderGetLogicalSize(ren, &lw, &lh);
    SDL_RenderSetLogicalSize(ren, 0, 0);                    // draw in real pixels so the gear stays crisp
    int ow, oh; SDL_GetRendererOutputSize(ren, &ow, &oh);
    float size = oh * 0.075f, m = oh * 0.03f;
    SDL_FRect r{ow - m - size, oh - m - size, size, size};
    SDL_SetTextureAlphaMod(gear, (Uint8)(alpha * 255));
    double angle = t * 140.0 + 30.0 * sin(t * 6.0) * (t < G_IN + G_HOLD ? 1 : 0);   // steady spin with a little tick
    SDL_RenderCopyExF(ren, gear, nullptr, &r, angle, nullptr, SDL_FLIP_NONE);
    SDL_RenderSetLogicalSize(ren, lw, lh);
}

// ---------------------------------------------------------------- filter worker (runs beside the emulator, 1 frame behind)
#define HI_W 4096
struct FilterJob {
    std::mutex m; std::condition_variable cv;
    std::vector<uint32_t> in, out;
    int w = 0, h = 0, mode = 0, dh = 0, dw = 0;   // dh/dw: output size for the full-res filters (the window's picture size)
    bool pending = false, done = false, quit = false;
    int ow = 0, oh = 0;
};
static void filter_worker(FilterJob *J) {
    std::vector<uint32_t> in, out;
    for (;;) {
        int w, h, mode, dh, dw;
        {
            std::unique_lock<std::mutex> l(J->m);
            J->cv.wait(l, [&] { return J->pending || J->quit; });
            if (J->quit) return;
            in.swap(J->in); w = J->w; h = J->h; mode = J->mode; dh = J->dh; dw = J->dw; J->pending = false;
        }
        int oh = mode == FILTER_XBRZ ? h * 4 : dh, ow = mode == FILTER_XBRZ ? w * 4 : dw;
        out.resize((size_t)ow * oh);
        if (mode == FILTER_XBRZ) filters::xbr4(in.data(), w, h, out.data());
        else if (mode == FILTER_SMOOTH) filters::smooth(in.data(), w, h, out.data(), dh, dw, 0);
        else if (mode == FILTER_LCD) filters::smooth(in.data(), w, h, out.data(), dh, dw, 1);
        else if (mode == FILTER_GLOW) filters::glow(in.data(), w, h, out.data(), dh, dw);
        else filters::crt(in.data(), w, h, out.data(), dh, dw);
        {
            std::lock_guard<std::mutex> l(J->m);
            J->out.swap(out); J->ow = ow; J->oh = oh; J->done = true;
        }
    }
}


// ---------------------------------------------------------------- flight recorder
// Everything that feeds the emulation is written per frame to replay_last.rpl (the previous run is kept as
// replay_previous.rpl). Header: "SKYRPL1\0", u32 save size, the save the run started from, u32 settings.
// Frame: u16 keys, u8 flags (1 touch, 2 widescreen), u8 x, u8 y, u8 enhancements mask. Lets a crash be replayed exactly.
static FILE *g_rec = nullptr;
static std::vector<uint8_t> g_recbuf;
static void rec_start(const std::string &dir, const std::vector<uint8_t> &save) {
    std::string a = dir + "replay_last.rpl", b = dir + "replay_previous.rpl";
    remove(b.c_str()); rename(a.c_str(), b.c_str());
    g_rec = fopen(a.c_str(), "wb");
    if (!g_rec) return;
    fwrite("SKYRPL1", 1, 8, g_rec);
    uint32_t n = (uint32_t)save.size(); fwrite(&n, 4, 1, g_rec);
    if (n) fwrite(save.data(), 1, n, g_rec);
    uint32_t st = 0; fwrite(&st, 4, 1, g_rec);
    fflush(g_rec);
}
static void rec_frame(uint16_t keys, bool touch, int x, int y, bool wide) {
    if (!g_rec) return;
    int m = 0; for (int i = 0; i < SADIE_COUNT; i++) if (recomp_settings.sadie[i]) m |= 1 << i;
    uint8_t f[6] = {(uint8_t)keys, (uint8_t)(keys >> 8), (uint8_t)((touch ? 1 : 0) | (wide ? 2 : 0)), (uint8_t)x, (uint8_t)y, (uint8_t)m};
    g_recbuf.insert(g_recbuf.end(), f, f + 6);
    if (g_recbuf.size() >= 6 * 60) { fwrite(g_recbuf.data(), 1, g_recbuf.size(), g_rec); fflush(g_rec); g_recbuf.clear(); }
}
static void rec_flush() { if (g_rec && !g_recbuf.empty()) { fwrite(g_recbuf.data(), 1, g_recbuf.size(), g_rec); fflush(g_rec); g_recbuf.clear(); } }

int main(int argc, char **argv) {
    {
        char *b = SDL_GetBasePath();
        std::string base = b ? b : "./";
        std::string lp = base + "recomp_log.txt", prev = base + "recomp_log_previous.txt";
        SDL_free(b);
        remove(prev.c_str());
        rename(lp.c_str(), prev.c_str());     // keep the last run's log (e.g. a crash) when relaunching
        freopen(lp.c_str(), "w", stderr);
        setvbuf(stderr, nullptr, _IONBF, 0);
        fprintf(stderr, "Sky Recomp log\n");
    }
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_handler);
    // vectored handler: sees fatal faults even on fiber stacks / before anything else can swallow them
    AddVectoredExceptionHandler(1, [](EXCEPTION_POINTERS *ep) -> LONG {
        DWORD c = ep->ExceptionRecord->ExceptionCode;
        if (c == 0xC0000005 || c == 0xC00000FD || c == 0xC000001D || c == 0xC0000409 || c == 0xC0000374) {
            static int once = 0;
            if (!once++) {
                fprintf(stderr, "\n*** FATAL (vectored) exception %08lX at %p\n", c, ep->ExceptionRecord->ExceptionAddress);
                crash_handler(ep);
                TerminateProcess(GetCurrentProcess(), 4);
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    });
#endif
    atexit([] { rec_flush(); fprintf(stderr, "[exit] program exited normally\n"); });
#ifdef _WIN32
    { ULONG g = 128 * 1024; SetThreadStackGuarantee(&g); }   // room for the crash report if a stack overflows
#endif
    signal(SIGABRT, on_abort);
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) fatal(SDL_GetError());

    // ---- find the ROM: command line, or any .nds next to the exe that is the clean USA dump
    std::string dir = exe_dir(), rom_path;
    std::vector<uint8_t> rom;
    std::vector<std::string> candidates;
    if (argc > 1) candidates.push_back(argv[1]);
    candidates.push_back(dir + "PMD_Explorers_of_Sky_USA.nds");
#ifdef _WIN32
    {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((dir + "*.nds").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do candidates.push_back(dir + fd.cFileName); while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
#endif
    for (auto &c : candidates) {
        auto data = read_file(c);
        if (!data.empty() && rom_is_vanilla(data.data(), data.size())) { rom = std::move(data); rom_path = c; break; }
    }
    if (rom.empty())
        fatal("Couldn't find your Explorers of Sky ROM.\n\n"
              "Put your clean (unpatched) USA dump of\n"
              "Pokemon Mystery Dungeon: Explorers of Sky\n"
              "in the same folder as SkyRecomp.exe.\n\n"
              "Patched ROMs won't work: the recompiled code is built from the vanilla game.");

    g_ini_path = dir + "SkyRecomp.ini";
    settings_load();
    settings_save();
    RecompSettings &S = recomp_settings;

    // ---- save file next to the ROM (slot 1 = "<rom>.sav", slots 2/3 = "<rom>_slot2.sav" / "_slot3.sav")
    std::string rom_base = rom_path.substr(0, rom_path.find_last_of('.'));
    std::string save_path = S.save_slot == 1 ? rom_base + ".sav" : rom_base + "_slot" + std::to_string(S.save_slot) + ".sav";
    auto save = read_file(save_path);
#ifdef _WIN32
    if (save.empty() && S.save_slot == 1) {
        // no "<rom>.sav": use the only .sav in the folder, if there is exactly one
        std::vector<std::string> savs;
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((dir + "*.sav").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do savs.push_back(dir + fd.cFileName); while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        if (savs.size() == 1) { save_path = savs[0]; save = read_file(save_path); }
    }
#endif
    Platform::g_save_path = save_path;
    rec_start(dir, save);
    std::string save_name = save_path.substr(save_path.find_last_of("/\\") + 1);
    std::string title = std::string("Explorers of Sky - Sky Recomp v0.7.2 - slot ") + std::to_string(S.save_slot) + " - " +
                        (save.empty() ? "new save: " : "save loaded: ") + save_name;

    // ---- set up the DS
    NDSArgs args;
    args.JIT = std::nullopt;
    auto ndsp = std::make_unique<NDS>(std::move(args));
    NDS &nds = *ndsp;
    auto srp = std::make_unique<SoftRenderer>(nds);
    SoftRenderer *sr = srp.get();
    nds.SetRenderer(std::move(srp));
    { RendererSettings rs{}; rs.ScaleFactor = 1; rs.Threaded = !getenv("NOTHREAD3D"); sr->SetRenderSettings(rs); }   // 3D on its own thread
    const int WM = 43;                 // 256 + 2*43 = 342 wide = 16:9 at 192 high
    bool wide = S.widescreen != 0;
    sr->WideMargin = wide ? WM : 0;
    recomp_wide_margin = wide ? WM : 0;
    NDSCart::NDSCartArgs cargs;
    if (!save.empty()) {
        cargs.SRAM = std::make_unique<u8[]>(save.size());
        memcpy(cargs.SRAM.get(), save.data(), save.size());
        cargs.SRAMLength = (u32)save.size();
    }
    auto romcopy = std::make_unique<u8[]>(rom.size());
    memcpy(romcopy.get(), rom.data(), rom.size());
    auto cart = NDSCart::ParseROM(std::move(romcopy), (u32)rom.size(), nullptr, std::move(cargs));
    if (!cart) fatal("melonDS couldn't read the ROM.");
    nds.SetNDSCart(std::move(cart));
    nds.Reset();
    nds.SetupDirectBoot("SkyRecomp");
    recomp_install(&nds);
    nds.Start();
    SDL_CreateThread([](void *) -> int { watchdog_thread(); return 0; }, "watchdog", nullptr);

    // ---- window: single-screen (4:3) by default
    enum Layout { SINGLE, STACKED } layout = SINGLE;
    bool pip = false;              // small view of the other screen in single-screen mode
    int force_screen = -1;         // -1 auto, 0 top, 1 bottom
    SDL_DisplayMode dm{};
    SDL_GetDesktopDisplayMode(0, &dm);
    const Res &res0 = RESOLUTIONS[S.resolution];
    SDL_Window *win = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       res0.w, res0.h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    const int FW = 256 + 2 * WM;
    SDL_Texture *tex_top = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, FW, 192);
    SDL_Texture *tex_bot = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, FW, 192);
    SDL_Texture *tex_hi = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, HI_W, 2160);
    SDL_SetTextureScaleMode(tex_hi, SDL_ScaleModeLinear);
    int hi_w = 0, hi_h = 0;               // size of the filtered image currently in tex_hi (0 = none yet)
    SDL_Texture *gear_tex = make_gear_texture(ren);
    int gear_seen = Platform::g_save_writes; uint64_t gear_start = 0; bool gear_on = false;
    FilterJob fjob;
    std::thread fthread(filter_worker, &fjob);
    SDL_RenderSetLogicalSize(ren, FW, 192);
    // size the window to the chosen resolution, keeping the picture's aspect (the window is the picture)
    auto apply_layout = [&]() {
        int vw = wide ? FW : 256, vh = layout == SINGLE ? 192 : 384;
        SDL_RenderSetLogicalSize(ren, vw, vh);
        Uint32 fl = SDL_GetWindowFlags(win);
        if (S.display == DISPLAY_WINDOWED && !(fl & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP))) {
            const Res &r = RESOLUTIONS[S.resolution];
            int h = r.h, w = h * vw / vh;
            if (w > r.w) { w = r.w; h = w * vh / vw; }
            if (dm.w > 0 && (w > dm.w || h > dm.h - 60)) { h = dm.h - 60; w = h * vw / vh; }
            SDL_SetWindowSize(win, w, h);
            SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
    };
    auto apply_display = [&]() {
        if (S.display == DISPLAY_WINDOWED) {
            SDL_SetWindowFullscreen(win, 0);
        } else if (S.display == DISPLAY_BORDERLESS) {
            SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
        } else {
            SDL_DisplayMode want{SDL_PIXELFORMAT_UNKNOWN, RESOLUTIONS[S.resolution].w, RESOLUTIONS[S.resolution].h, 0, nullptr}, got{};
            SDL_SetWindowFullscreen(win, 0);
            if (SDL_GetClosestDisplayMode(0, &want, &got)) SDL_SetWindowDisplayMode(win, &got);
            SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN);
        }
        apply_layout();
    };
    apply_display();
    // which screen is "the game": the bottom one (engine A), except when the bottom
    // goes dark while the top is showing something (e.g. the world-map travel scene).
    bool main_top = false;
    int dark_frames = 0, lit_frames = 0;
    auto mostly_dark = [](const uint32_t *fb) {
        int dark = 0, n = 0;
        for (int i = 0; i < 256 * 192; i += 7, n++) {
            uint32_t p = fb[i];
            if (((p >> 16) & 0xFF) + ((p >> 8) & 0xFF) + (p & 0xFF) < 30) dark++;
        }
        return dark * 100 > n * 97;
    };

    // ---- audio
    SDL_AudioSpec want{}, have{};
    want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    SDL_AudioDeviceID audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (audio) { nds.SPU.SetOutputSampleRate(have.freq); SDL_PauseAudioDevice(audio, 0); }
    std::vector<s16> abuf(48000);

    SDL_GameController *pad = nullptr;
    u32 keys = 0;          // bits set = pressed
    bool running = true, fast_key = false, fast_pad = false, fast = false;
    const double frame_ms = 1000.0 / 59.8261;
    uint64_t freq = SDL_GetPerformanceFrequency(), next = SDL_GetPerformanceCounter();
    bool touching = false;
    int touch_x = 0, touch_y = 0;
    int qs_request = 0, ql_request = 0;
    Uint32 toast_until = 0;
    std::string qs_path = rom_base + "_slot" + std::to_string(S.save_slot) + ".quicksave";
    auto toast = [&](const char *msg) {
        SDL_SetWindowTitle(win, (title + "   --   " + msg).c_str());
        toast_until = SDL_GetTicks() + 2500;
    };
    std::vector<uint32_t> frame_top(FW * 192), frame_bot(FW * 192);
    bool have_frame = false;

    auto key_bit = [](SDL_Keycode k) -> int {
        switch (k) {
            case SDLK_x: return K_A;      case SDLK_z: return K_B;
            case SDLK_s: return K_X;      case SDLK_a: return K_Y;
            case SDLK_q: return K_L;      case SDLK_w: return K_R;
            case SDLK_RETURN: return K_START; case SDLK_RSHIFT: case SDLK_BACKSPACE: return K_SELECT;
            case SDLK_UP: return K_UP;    case SDLK_DOWN: return K_DOWN;
            case SDLK_LEFT: return K_LEFT; case SDLK_RIGHT: return K_RIGHT;
        }
        return -1;
    };

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_QUIT: running = false; break;
                case SDL_KEYDOWN: case SDL_KEYUP: {
                    if (e.key.keysym.sym == SDLK_TAB) { fast_key = e.type == SDL_KEYDOWN; break; }
                    if (e.key.keysym.sym == SDLK_F11 && e.type == SDL_KEYDOWN) {
                        S.display = S.display == DISPLAY_WINDOWED ? DISPLAY_BORDERLESS : DISPLAY_WINDOWED;
                        S.dirty = 1;
                        break;
                    }
                    if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                        if (e.key.keysym.sym == SDLK_F1) { layout = layout == SINGLE ? STACKED : SINGLE; apply_layout(); break; }
                        if (e.key.keysym.sym == SDLK_F2) { pip = !pip; break; }
#ifdef SKYRECOMP_DEV   // dev-only keys: quicksave/quickload and the save-gear test (build with DEV=1)
                        if (e.key.keysym.sym == SDLK_F5) { qs_request = 1; break; }
                        if (e.key.keysym.sym == SDLK_F6) { gear_start = SDL_GetPerformanceCounter(); gear_on = true; break; }
                        if (e.key.keysym.sym == SDLK_F9) { ql_request = 1; break; }
#endif
                        if (e.key.keysym.sym == SDLK_F3) { force_screen = force_screen == -1 ? (main_top ? 1 : 0) : -1; break; }
                    }
                    int b = key_bit(e.key.keysym.sym);
                    if (b >= 0) { if (e.type == SDL_KEYDOWN) keys |= 1u << b; else keys &= ~(1u << b); }
                    break;
                }
                case SDL_CONTROLLERBUTTONDOWN:
                    if (e.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK) pip = !pip;
                    if (e.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSTICK) force_screen = force_screen == -1 ? (main_top ? 1 : 0) : -1;
                    break;
                case SDL_CONTROLLERDEVICEADDED:
                    if (!pad) pad = SDL_GameControllerOpen(e.cdevice.which);
                    break;
                case SDL_CONTROLLERDEVICEREMOVED:
                    if (pad && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) { SDL_GameControllerClose(pad); pad = nullptr; }
                    break;
                case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONUP: {
                    if (e.type == SDL_MOUSEBUTTONUP) { touching = false; break; }
                    if (e.type == SDL_MOUSEMOTION && !touching) break;
                    int mx = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x;
                    int my = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y;
                    if (wide) mx -= WM;
                    int tx = -1, ty = -1;
                    if (layout == STACKED) { if (my >= 192 && my < 384) { tx = mx; ty = my - 192; } }
                    else if (!main_top) { tx = mx; ty = my; }      // single-screen showing the touch screen
                    if (tx >= 0 && tx < 256 && ty >= 0 && ty < 192) { touching = true; touch_x = tx; touch_y = ty; }
                    break;
                }
            }
        }

        u32 padkeys = 0;
        if (pad) {
            auto btn = [&](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(pad, b) != 0; };
            if (btn(SDL_CONTROLLER_BUTTON_A)) padkeys |= 1u << K_A;
            if (btn(SDL_CONTROLLER_BUTTON_B)) padkeys |= 1u << K_B;
            if (btn(SDL_CONTROLLER_BUTTON_X)) padkeys |= 1u << K_Y;
            if (btn(SDL_CONTROLLER_BUTTON_Y)) padkeys |= 1u << K_X;
            if (btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) padkeys |= 1u << K_L;
            if (btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) padkeys |= 1u << K_R;
            if (btn(SDL_CONTROLLER_BUTTON_START)) padkeys |= 1u << K_START;
            if (btn(SDL_CONTROLLER_BUTTON_BACK)) padkeys |= 1u << K_SELECT;
            if (btn(SDL_CONTROLLER_BUTTON_DPAD_UP)) padkeys |= 1u << K_UP;
            if (btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) padkeys |= 1u << K_DOWN;
            if (btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) padkeys |= 1u << K_LEFT;
            if (btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) padkeys |= 1u << K_RIGHT;
            int lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
            int ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
            const int dz = 12000;
            if (lx < -dz) padkeys |= 1u << K_LEFT;
            if (lx > dz) padkeys |= 1u << K_RIGHT;
            if (ly < -dz) padkeys |= 1u << K_UP;
            if (ly > dz) padkeys |= 1u << K_DOWN;
            fast_pad = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
        }
        fast = fast_key || fast_pad;
        nds.SetKeyMask(~(keys | padkeys) & 0xFFF);
        if (S.dirty) {
            S.dirty = 0;
            if ((S.widescreen != 0) != wide) { wide = S.widescreen != 0; sr->WideMargin = wide ? WM : 0; recomp_wide_margin = wide ? WM : 0; }
            hi_w = 0;
            apply_display();
            settings_save();
        }
        // ---- quicksave (F5) / quickload (F9): only at a moment the game is idling in its wait-for-interrupt
        if (qs_request && !recomp_menu_busy() && recomp_quicksave_prepare()) {
            qs_request = 0;
            Savestate st;
            nds.DoSavestate(&st);
            st.Finish();
            std::string tmp = qs_path + ".tmp";
            FILE *qf = fopen(tmp.c_str(), "wb");
            bool ok = qf && fwrite(st.Buffer(), 1, st.Length(), qf) == st.Length();
            if (qf) fclose(qf);
            if (ok) { remove(qs_path.c_str()); rename(tmp.c_str(), qs_path.c_str()); }
            fprintf(stderr, "[quicksave] %s (%u bytes)\n", ok ? "saved" : "FAILED", st.Length());
            toast(ok ? "Quicksaved (F9 to load)" : "Quicksave failed");
        } else if (qs_request && ++qs_request > 600) { qs_request = 0; toast("Couldn't quicksave right now - try again"); }
        if (ql_request) {
            ql_request = 0;
            auto buf = read_file(qs_path);
            if (buf.empty()) toast("No quicksave yet (F5 to make one)");
            else {
                Savestate st(buf.data(), (u32)buf.size(), false);
                if (nds.DoSavestate(&st) && !st.Error) {
                    recomp_menu_reset();
                    recomp_quickload_reset();
                    fprintf(stderr, "[quickload] loaded %zu bytes\n", buf.size());
                    toast("Quickloaded");
                } else toast("Quickload failed");
            }
        }
        if (toast_until && SDL_GetTicks() > toast_until) { toast_until = 0; SDL_SetWindowTitle(win, title.c_str()); }
        sadie_sync();
        // dungeon darkness is translucent 3D that should carry into the margins; overworld 3D (cloud shadows) must not
        sr->WideExtend3D = recomp_cpu_ready() && recomp_overlay_loaded(29);
        {   // dialogue portraits sit left of the text box in widescreen
            int pmv[14] = {0}; recomp_portrait_move(pmv);
            sr->PortMove.n = pmv[0]; sr->PortMove.eng = pmv[1] ? 1 : 0;
            for (int k = 0; k < 2; k++) {
                auto& r = sr->PortMove.r[k]; const int* v = pmv + 2 + k * 6;
                r.sx = v[0]; r.sy = v[1]; r.sw = v[2]; r.sh = v[3]; r.dx = v[4]; r.dy = v[5];
            }
        }
        if (S.request_exit) running = false;
        if (S.request_slot) {
            // switching save slot: remember it and restart the program on that slot
            S.save_slot = S.request_slot; S.request_slot = 0;
            settings_save();
#ifdef _WIN32
            char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
            STARTUPINFOA si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
            std::string cmd = std::string("\"") + exe + "\"";
            if (CreateProcessA(exe, (char *)cmd.c_str(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
#endif
            running = false;
        }

        if (touching) nds.TouchScreen(touch_x, touch_y); else nds.ReleaseScreen();
        rec_frame((uint16_t)((keys | padkeys) & 0xFFF), touching, touch_x, touch_y, recomp_wide_margin != 0);
        nds.RunFrame();
        g_frames++;
        {
            extern int recomp_post_reset_log;
            static int post_frames = 0;
            if (recomp_post_reset_log > 0 || (post_frames > 0 && post_frames < 1200)) {
                post_frames++;
                if (post_frames % 30 == 1) {
                    fprintf(stderr, "[post-reset] frame %llu (%d since reset) ring:", (unsigned long long)g_frames, post_frames);
                    for (unsigned i = 0; i < 8; i++) fprintf(stderr, " %08X", fn_ring[(fn_ring_pos - 8 + i) & 255]);
                    fprintf(stderr, "\n");
                    void recomp_arm7_state(FILE *); recomp_arm7_state(stderr);
                }
            }
        }
        if (g_frames % 1800 == 0) { fprintf(stderr, "[frame %llu] ", (unsigned long long)g_frames); recomp_stats(stderr); }

        void *top, *bot;
        if (nds.GPU.GetFramebuffers(&top, &bot)) {
            void *pix; int pitch;
            for (int scr = 0; scr < 2; scr++) {
                uint32_t *c = (uint32_t *)(scr ? bot : top), *L, *Rm;
                sr->GetWideMargins(scr, &L, &Rm);
                std::vector<uint32_t> &fr = scr ? frame_bot : frame_top;
                for (int y = 0; y < 192; y++) {
                    uint32_t *row = fr.data() + y * FW;
                    if (wide) { memcpy(row, L + y * WM, WM * 4); memcpy(row + WM + 256, Rm + y * WM, WM * 4); }
                    else { memset(row, 0, WM * 4); memset(row + WM + 256, 0, WM * 4); }
                    memcpy(row + WM, c + y * 256, 1024);
                }
                SDL_LockTexture(scr ? tex_bot : tex_top, nullptr, &pix, &pitch);
                for (int y = 0; y < 192; y++) memcpy((uint8_t *)pix + y * pitch, fr.data() + y * FW, FW * 4);
                SDL_UnlockTexture(scr ? tex_bot : tex_top);
                have_frame = true;
            }
            bool bot_dark = mostly_dark((uint32_t *)bot), top_dark = mostly_dark((uint32_t *)top);
            if (bot_dark && !top_dark) { dark_frames++; lit_frames = 0; }
            else if (!bot_dark) { lit_frames++; dark_frames = 0; }
            else { lit_frames++; dark_frames = 0; }      // both dark (fades, story text on black): the game screen wins
            if (!main_top && dark_frames >= 4) main_top = true;
            if (main_top && lit_frames >= 2) main_top = false;
        }
        bool show_top = force_screen == -1 ? main_top : force_screen == 0;
        // only draw widescreen margins for screens that are on view (bit 0 top, bit 1 bottom)
        sr->WideScreens = (layout != SINGLE || pip) ? 3 : (show_top ? 1 : 2);
        int vw = wide ? FW : 256, vx0 = wide ? 0 : WM;
        if (S.filter != FILTER_OFF) {
            // hand the visible picture to the filter thread; pick up its last result
            std::unique_lock<std::mutex> l(fjob.m);
            if (fjob.done) {
                void *pix; int pitch;
                if (SDL_LockTexture(tex_hi, nullptr, &pix, &pitch) == 0) {
                    for (int y = 0; y < fjob.oh; y++) memcpy((uint8_t *)pix + y * pitch, fjob.out.data() + (size_t)y * fjob.ow, fjob.ow * 4);
                    SDL_UnlockTexture(tex_hi);
                    hi_w = fjob.ow; hi_h = fjob.oh;
                }
                fjob.done = false;
            }
            if (!fjob.pending && have_frame) {
                int vh = layout == SINGLE ? 192 : 384;
                fjob.in.resize((size_t)vw * vh);
                for (int scr = 0; scr < (layout == SINGLE ? 1 : 2); scr++) {
                    const uint32_t *src = layout == SINGLE ? (show_top ? frame_top.data() : frame_bot.data()) : (scr ? frame_bot.data() : frame_top.data());
                    for (int y = 0; y < 192; y++) memcpy(&fjob.in[(size_t)(scr * 192 + y) * vw], src + y * FW + vx0, vw * 4);
                }
                int ow, oh2; SDL_GetRendererOutputSize(ren, &ow, &oh2);
                int pic_h = oh2; if ((long long)ow * vh < (long long)oh2 * vw) pic_h = (int)((long long)ow * vh / vw);
                if (pic_h > 2160) pic_h = 2160; if (pic_h < vh) pic_h = vh;
                int pic_w = (int)((long long)pic_h * vw / vh); if (pic_w > HI_W) pic_w = HI_W;
                fjob.w = vw; fjob.h = vh; fjob.mode = S.filter; fjob.dh = pic_h; fjob.dw = pic_w; fjob.pending = true;
                l.unlock(); fjob.cv.notify_one();
            }
        } else hi_w = 0;

        if (audio) {
            int n = nds.SPU.GetOutputSize();
            if (n > (int)abuf.size() / 2) n = abuf.size() / 2;
            n = nds.SPU.ReadOutput(abuf.data(), n);
            if (!fast && SDL_GetQueuedAudioSize(audio) < (Uint32)have.freq)   // cap latency at ~1s worth
                SDL_QueueAudio(audio, abuf.data(), n * 4);
        }

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_Rect src{vx0, 0, vw, 192};
        if (hi_w > 0 && hi_h > 0 && (S.filter != FILTER_XBRZ || (hi_w == vw * 4 && hi_h == (layout == SINGLE ? 192 : 384) * 4))) {
            SDL_Rect hs{0, 0, hi_w, hi_h};
            SDL_RenderCopy(ren, tex_hi, &hs, nullptr);
            if (layout == SINGLE && pip) {
                SDL_Rect r{vw - 88, 4, 84, 63}, frame{r.x - 1, r.y - 1, r.w + 2, r.h + 2};
                SDL_Rect psrc{WM, 0, 256, 192};
                SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
                SDL_RenderFillRect(ren, &frame);
                SDL_RenderCopy(ren, show_top ? tex_bot : tex_top, &psrc, &r);
            }
        } else if (layout == STACKED) {
            SDL_Rect a{0, 0, vw, 192}, b{0, 192, vw, 192};
            SDL_RenderCopy(ren, tex_top, &src, &a);
            SDL_RenderCopy(ren, tex_bot, &src, &b);
        } else {
            SDL_RenderCopy(ren, show_top ? tex_top : tex_bot, &src, nullptr);
            if (pip) {
                SDL_Rect r{vw - 88, 4, 84, 63}, frame{r.x - 1, r.y - 1, r.w + 2, r.h + 2};
                SDL_Rect psrc{WM, 0, 256, 192};
                SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
                SDL_RenderFillRect(ren, &frame);
                SDL_RenderCopy(ren, show_top ? tex_bot : tex_top, &psrc, &r);
            }
        }
        if (!fast) {
            // pace to the DS refresh rate (vsync alone would run 60Hz/144Hz monitors at the wrong speed)
            next += (uint64_t)(frame_ms * freq / 1000.0);
            uint64_t now = SDL_GetPerformanceCounter();
            if (now < next) SDL_Delay((Uint32)((next - now) * 1000 / freq));
            else if (now - next > freq / 4) next = now;
        } else next = SDL_GetPerformanceCounter();
        if (Platform::g_save_writes != gear_seen) { gear_seen = Platform::g_save_writes; gear_start = SDL_GetPerformanceCounter(); gear_on = true; }
        if (gear_on) {
            double gt = (double)(SDL_GetPerformanceCounter() - gear_start) / SDL_GetPerformanceFrequency();
            if (gt > 3.5) gear_on = false; else draw_gear(ren, gear_tex, gt);
        }
        SDL_RenderPresent(ren);
    }

    { std::lock_guard<std::mutex> l(fjob.m); fjob.quit = true; }
    fjob.cv.notify_one();
    fthread.join();
    settings_save();
    nds.Stop();
    SDL_Quit();
    return 0;
}
