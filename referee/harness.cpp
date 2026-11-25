#include <vector>
// Headless melonDS referee: boots a ROM via FreeBIOS direct boot, runs frames,
// dumps screenshots (PPM) and main RAM for comparing against the recomp.
#include <cstdio>
#include "settings.h"
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <vector>
#include <algorithm>
#include "NDS.h"
#include "settings.h"
#include "NDSCart.h"
#include "Savestate.h"
#include "Platform.h"
#include "GPU_Soft.h"
#include "Args.h"

using namespace melonDS;
void recomp_install(NDS*);
void recomp_stats(FILE*);
extern int recomp_inject_reset;
void recomp_arm7_state(FILE*);

// ---------------- Platform stubs ----------------
namespace melonDS::Platform {
void SignalStop(StopReason, void*) {}
std::string GetLocalFilePath(const std::string& f) { return f; }
FileHandle* OpenFile(const std::string& path, FileMode mode) {
    const char* m = (mode & FileMode::Write) ? ((mode & FileMode::Read) ? "r+b" : "wb") : "rb";
    return (FileHandle*)fopen(path.c_str(), m);
}
FileHandle* OpenLocalFile(const std::string& p, FileMode m) { return OpenFile(p, m); }
bool FileExists(const std::string& n) { FILE* f = fopen(n.c_str(), "rb"); if (f) fclose(f); return f; }
bool LocalFileExists(const std::string& n) { return FileExists(n); }
bool CheckFileWritable(const std::string&) { return true; }
bool CheckLocalFileWritable(const std::string&) { return true; }
bool CloseFile(FileHandle* f) { return fclose((FILE*)f) == 0; }
bool IsEndOfFile(FileHandle* f) { return feof((FILE*)f); }
bool FileReadLine(char* s, int c, FileHandle* f) { return fgets(s, c, (FILE*)f); }
u64 FilePosition(FileHandle* f) { return ftell((FILE*)f); }
bool FileSeek(FileHandle* f, s64 o, FileSeekOrigin w) { return fseek((FILE*)f, o, w == FileSeekOrigin::Start ? SEEK_SET : w == FileSeekOrigin::Current ? SEEK_CUR : SEEK_END) == 0; }
void FileRewind(FileHandle* f) { rewind((FILE*)f); }
u64 FileRead(void* d, u64 s, u64 c, FileHandle* f) { return fread(d, s, c, (FILE*)f); }
bool FileFlush(FileHandle* f) { return fflush((FILE*)f) == 0; }
u64 FileWrite(const void* d, u64 s, u64 c, FileHandle* f) { return fwrite(d, s, c, (FILE*)f); }
u64 FileWriteFormatted(FileHandle* f, const char* fmt, ...) { va_list a; va_start(a, fmt); int r = vfprintf((FILE*)f, fmt, a); va_end(a); return r; }
u64 FileLength(FileHandle* f) { long p = ftell((FILE*)f); fseek((FILE*)f, 0, SEEK_END); long l = ftell((FILE*)f); fseek((FILE*)f, p, SEEK_SET); return l; }
void Log(LogLevel lv, const char* fmt, ...) { if (lv < LogLevel::Warn && !getenv("REF_VERBOSE")) return; va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); }
struct Thread { std::thread t; };
Thread* Thread_Create(std::function<void()> fn) { auto* t = new Thread; t->t = std::thread(fn); return t; }
void Thread_Free(Thread* t) { if (t->t.joinable()) t->t.detach(); delete t; }
void Thread_Wait(Thread* t) { if (t->t.joinable()) t->t.join(); }
struct Semaphore { std::mutex m; std::condition_variable cv; int n = 0; };
Semaphore* Semaphore_Create() { return new Semaphore; }
void Semaphore_Free(Semaphore* s) { delete s; }
void Semaphore_Reset(Semaphore* s) { std::lock_guard<std::mutex> l(s->m); s->n = 0; }
void Semaphore_Wait(Semaphore* s) { std::unique_lock<std::mutex> l(s->m); s->cv.wait(l, [&] { return s->n > 0; }); s->n--; }
bool Semaphore_TryWait(Semaphore* s, int ms) { std::unique_lock<std::mutex> l(s->m); if (!s->cv.wait_for(l, std::chrono::milliseconds(ms), [&] { return s->n > 0; })) return false; s->n--; return true; }
void Semaphore_Post(Semaphore* s, int c) { { std::lock_guard<std::mutex> l(s->m); s->n += c; } s->cv.notify_all(); }
struct Mutex { std::mutex m; };
Mutex* Mutex_Create() { return new Mutex; }
void Mutex_Free(Mutex* m) { delete m; }
void Mutex_Lock(Mutex* m) { m->m.lock(); }
void Mutex_Unlock(Mutex* m) { m->m.unlock(); }
bool Mutex_TryLock(Mutex* m) { return m->m.try_lock(); }
void Sleep(u64 us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
u64 GetMSCount() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
u64 GetUSCount() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void WriteNDSSave(const u8*, u32, u32, u32, void*) {}
void WriteGBASave(const u8*, u32, u32, u32, void*) {}
void WriteFirmware(const Firmware&, u32, u32, void*) {}
void WriteDateTime(int, int, int, int, int, int, void*) {}
void MP_Begin(void*) {}
void MP_End(void*) {}
int MP_SendPacket(u8*, int, u64, void*) { return 0; }
int MP_RecvPacket(u8*, u64*, void*) { return 0; }
int MP_SendCmd(u8*, int, u64, void*) { return 0; }
int MP_SendReply(u8*, int, u64, u16, void*) { return 0; }
int MP_SendAck(u8*, int, u64, void*) { return 0; }
int MP_RecvHostPacket(u8*, u64*, void*) { return 0; }
u16 MP_RecvReplies(u8*, u64, u16, void*) { return 0; }
int Net_SendPacket(u8*, int, void*) { return 0; }
int Net_RecvPacket(u8*, void*) { return 0; }
void Camera_Start(int, void*) {}
void Camera_Stop(int, void*) {}
void Camera_CaptureFrame(int, u32*, int, int, bool, void*) {}
void Mic_Start(void*) {}
void Mic_Stop(void*) {}
int Mic_ReadInput(s16*, int, void*) { return 0; }
AACDecoder* AAC_Init() { return nullptr; }
void AAC_DeInit(AACDecoder*) {}
bool AAC_Configure(AACDecoder*, int, int) { return false; }
bool AAC_DecodeFrame(AACDecoder*, const void*, int, void*, int) { return false; }
bool Addon_KeyDown(KeyType, void*) { return false; }
void Addon_RumbleStart(u32, void*) {}
void Addon_RumbleStop(void*) {}
float Addon_MotionQuery(MotionQueryType, void*) { return 0; }
DynamicLibrary* DynamicLibrary_Load(const char*) { return nullptr; }
void DynamicLibrary_Unload(DynamicLibrary*) {}
void* DynamicLibrary_LoadFunction(DynamicLibrary*, const char*) { return nullptr; }
}

static void save_ppm(const char* path, const u32* top, const u32* bot) {
    FILE* f = fopen(path, "wb");
    fprintf(f, "P6\n256 384\n255\n");
    for (int i = 0; i < 256 * 384; i++) {
        u32 p = i < 256 * 192 ? top[i] : bot[i - 256 * 192];
        unsigned char rgb[3] = {(unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

extern "C" uint32_t dbg_ground_o;
extern "C" int recomp_wide_margin;
extern "C" void sadie_sync(void);
extern "C" int recomp_quicksave_prepare(void);
extern "C" void recomp_quickload_reset(void);
extern "C" void recomp_portrait_move(int *);
extern "C" int recomp_overlay_loaded(int);
extern "C" int recomp_cpu_ready(void);

extern "C" void recomp_set_watch(uint32_t, uint32_t);
namespace melonDS { extern void (*ARM9WriteWatch)(u32, u32, int); extern void (*ARM9ReadWatch)(u32, int); }
extern uint32_t fn_ring[256]; extern unsigned fn_ring_pos;
static melonDS::NDS* g_nds;
static u32 grlo = 1, grhi = 0; static int grn = 0;
static void grwatch(u32 a, int sz) {
    if (a < grlo || a >= grhi || grn >= 60) return;
    grn++;
    fprintf(stderr, "R %08X lr=%08X ring:", a, g_nds->ARM9.R[14]);
    for (unsigned i = 0; i < 6; i++) fprintf(stderr, " %08X", fn_ring[(fn_ring_pos - 6 + i) & 255]);
    fprintf(stderr, "\n");
}
static u32 gwlo = 1, gwhi = 0; static int gwn = 0;
extern uint32_t fn_ring[256]; extern unsigned fn_ring_pos;
static void gwatch(u32 a, u32 v, int sz) {
    if (a < gwlo || a >= gwhi || gwn >= 3000) return;
    gwn++;
    int dma = 0; for (int i = 0; i < 4; i++) if (g_nds->ARM9Read16(0x040000BA + 12*i) & 0x8000) dma |= 1<<i;
    fprintf(stderr, "B %08X=%08X sz%d pc=%08X lr=%08X dma=%d ring:", a, v, sz, g_nds->ARM9.R[15], g_nds->ARM9.R[14], dma);
    for (unsigned i = 0; i < 40; i++) fprintf(stderr, " %08X", fn_ring[(fn_ring_pos - 40 + i) & 255]);
    fprintf(stderr, "\n");
}
int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: harness rom.nds frames outprefix [shot_every]\n"); return 1; }
    int frames = atoi(argv[2]);
    int every = argc > 4 ? atoi(argv[4]) : frames;
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    auto rom = std::make_unique<u8[]>(len);
    fread(rom.get(), 1, len, f); fclose(f);

    NDSArgs args;
    args.JIT = std::nullopt;
    auto ndsp = std::make_unique<NDS>(std::move(args));
    NDS& nds = *ndsp;
    auto srp = std::make_unique<SoftRenderer>(nds);
    SoftRenderer* sr = srp.get();
    nds.SetRenderer(std::move(srp));
    { RendererSettings rs{}; rs.ScaleFactor = 1; rs.Threaded = !getenv("NOTHREAD3D"); sr->SetRenderSettings(rs); }   // 3D on its own thread
    if (getenv("WIDESCR")) sr->WideScreens = atoi(getenv("WIDESCR"));
    int W = getenv("WIDE") ? atoi(getenv("WIDE")) : 0;
    recomp_wide_margin = W;
    if (const char* sd = getenv("SADIE")) for (int g = 0; g < 6 && sd[g]; g++) recomp_settings.sadie[g] = sd[g] == '1';
    sr->WideMargin = W;
    std::optional<NDSCart::NDSCartArgs> cargs;
    // REPLAY=file.rpl: a Sky Recomp flight recording (starting save + per-frame inputs/settings)
    std::vector<u8> rpl; size_t rpl_pos = 0, rpl_frames = 0;
    if (const char* rp = getenv("REPLAY")) {
        FILE* rf = fopen(rp, "rb");
        if (!rf) { fprintf(stderr, "no replay\n"); return 1; }
        fseek(rf, 0, SEEK_END); rpl.resize(ftell(rf)); fseek(rf, 0, SEEK_SET); fread(rpl.data(), 1, rpl.size(), rf); fclose(rf);
        u32 n; memcpy(&n, &rpl[8], 4);
        if (n) {
            NDSCart::NDSCartArgs a;
            a.SRAM = std::make_unique<u8[]>(n); memcpy(a.SRAM.get(), &rpl[12], n); a.SRAMLength = n;
            cargs = std::move(a);
        }
        rpl_pos = 12 + n + 4;
        rpl_frames = (rpl.size() - rpl_pos) / 6;
        fprintf(stderr, "replay: save %u bytes, %zu frames\n", n, rpl_frames);
        if (frames <= 0 || (size_t)frames > rpl_frames) frames = (int)rpl_frames;
    }
    if (const char* sp = getenv("SAVE")) {
        FILE* sf = fopen(sp, "rb");
        if (sf) {
            fseek(sf, 0, SEEK_END); long sl = ftell(sf); fseek(sf, 0, SEEK_SET);
            NDSCart::NDSCartArgs a;
            a.SRAM = std::make_unique<u8[]>(sl); fread(a.SRAM.get(), 1, sl, sf); fclose(sf);
            a.SRAMLength = (u32)sl;
            cargs = std::move(a);
            fprintf(stderr, "loaded save %ld bytes\n", sl);
        }
    }
    auto cart = NDSCart::ParseROM(std::move(rom), (u32)len, nullptr, std::move(cargs));
    if (!cart) { fprintf(stderr, "ROM parse failed\n"); return 1; }
    nds.SetNDSCart(std::move(cart));
    nds.Reset();
    nds.SetupDirectBoot("game.nds");
    if (getenv("RECOMP")) recomp_install(&nds);
    nds.Start();
    if (const char* lp = getenv("STATE_LOAD")) {
        FILE* sf = fopen(lp, "rb"); fseek(sf, 0, SEEK_END); long sl = ftell(sf); fseek(sf, 0, SEEK_SET);
        std::vector<u8> buf(sl); fread(buf.data(), 1, sl, sf); fclose(sf);
        Savestate st(buf.data(), (u32)sl, false);
        bool ok = nds.DoSavestate(&st);
        fprintf(stderr, "state load %s: %d\n", lp, ok);
    }
    for (int i = 1; i <= frames; i++) {
        static int qs_pending = 0;
        if (getenv("QS_AT") && i == atoi(getenv("QS_AT"))) qs_pending = 1;
        if (qs_pending && recomp_quicksave_prepare()) {
            qs_pending = 0;
            Savestate st; nds.DoSavestate(&st); st.Finish();
            FILE* sf = fopen(getenv("QS_FILE"), "wb"); fwrite(st.Buffer(), 1, st.Length(), sf); fclose(sf);
            fprintf(stderr, "quicksave at frame %d\n", i);
        }
        if (getenv("QL_AT") && i == atoi(getenv("QL_AT"))) {
            FILE* sf = fopen(getenv("QS_FILE"), "rb"); fseek(sf, 0, SEEK_END); long sl = ftell(sf); fseek(sf, 0, SEEK_SET);
            std::vector<u8> buf(sl); fread(buf.data(), 1, sl, sf); fclose(sf);
            Savestate st(buf.data(), (u32)sl, false);
            fprintf(stderr, "quickload at frame %d: %d\n", i, nds.DoSavestate(&st));
            recomp_quickload_reset();
        }
        if (getenv("STATE_SAVE_AT") && i == atoi(getenv("STATE_SAVE_AT")) + 1) {
            Savestate st; nds.DoSavestate(&st); st.Finish();
            FILE* sf = fopen(getenv("STATE_FILE"), "wb"); fwrite(st.Buffer(), 1, st.Length(), sf); fclose(sf);
            fprintf(stderr, "state saved (%u bytes)\n", st.Length());
        }
        // KEYS="from-to:key,..." key = A B SEL START R L U D RS LS X Y (DS bit order)
        u32 mask = 0;
        if (const char* ks = getenv("KEYS")) {
            std::string k = ks; size_t p = 0;
            while (p < k.size()) {
                size_t e = k.find(',', p); if (e == std::string::npos) e = k.size();
                int a, b; char name[16];
                if (sscanf(k.substr(p, e - p).c_str(), "%d-%d:%15s", &a, &b, name) == 3 && i >= a && i <= b) {
                    const char* names[] = {"A","B","SEL","START","RIGHT","LEFT","UP","DOWN","R","L","X","Y"};
                    for (int q = 0; q < 12; q++) if (!strcmp(name, names[q])) mask |= 1u << q;
                    if (!strcmp(name, "TOUCH")) mask |= 1u << 16;
                }
                p = e + 1;
            }
        }
        nds.SetKeyMask(~mask & 0xFFF);
        if (mask & (1u << 16)) nds.TouchScreen(128, 96); else nds.ReleaseScreen();
        if (!rpl.empty()) {
            const u8* r = &rpl[rpl_pos + (size_t)(i - 1) * 6];
            nds.SetKeyMask(~(r[0] | r[1] << 8) & 0xFFF);
            if (r[2] & 1) nds.TouchScreen(r[3], r[4]); else nds.ReleaseScreen();
            int w = (r[2] & 2) ? 43 : 0;
            recomp_wide_margin = w; sr->WideMargin = W ? w : 0;
            for (int g = 0; g < 6; g++) recomp_settings.sadie[g] = (r[5] >> g) & 1;
        }
        if (getenv("RWATCHAT") && i == atoi(getenv("RWATCHAT"))) { g_nds = &nds; grlo = strtoul(getenv("RLO"),0,16); grhi = strtoul(getenv("RHI"),0,16); melonDS::ARM9ReadWatch = grwatch; }
        if (getenv("WATCHAT") && i == atoi(getenv("WATCHAT"))) { g_nds = &nds; gwlo = strtoul(getenv("WLO"),0,16); gwhi = strtoul(getenv("WHI"),0,16); melonDS::ARM9WriteWatch = gwatch; }
        if (getenv("POKEAT") && i >= atoi(getenv("POKEAT")) && getenv("POKE8")) {   // POKE8="addr:val,addr:val" (hex), every frame from POKEAT
            char buf[512]; snprintf(buf, sizeof buf, "%s", getenv("POKE8"));
            for (char* t = strtok(buf, ","); t; t = strtok(nullptr, ",")) { u32 a = strtoul(t, 0, 16); char* c = strchr(t, ':'); if (c) nds.ARM9Write8(a, (u8)strtoul(c + 1, 0, 16)); }
        }
        if (getenv("STATS") && getenv("RECOMP") && i % 1800 == 0) { fprintf(stderr, "[frame %d] ", i); recomp_stats(stderr); }
        if (getenv("RECOMP")) { sadie_sync(); sr->WideExtend3D = recomp_cpu_ready() && recomp_overlay_loaded(29); }
        {   // dialogue portraits sit left of the text box in widescreen
            int pmv[14] = {0}; recomp_portrait_move(pmv);
            sr->PortMove.n = pmv[0]; sr->PortMove.eng = pmv[1] ? 1 : 0;
            for (int k = 0; k < 2; k++) {
                auto& r = sr->PortMove.r[k]; const int* v = pmv + 2 + k * 6;
                r.sx = v[0]; r.sy = v[1]; r.sw = v[2]; r.sh = v[3]; r.dx = v[4]; r.dy = v[5];
            }
        }
        if (getenv("RESETAT") && i == atoi(getenv("RESETAT"))) recomp_inject_reset = 1;
        if (getenv("RESETAT") && i > atoi(getenv("RESETAT")) && (i - atoi(getenv("RESETAT"))) % 30 == 1 && i < atoi(getenv("RESETAT")) + 400) { fprintf(stderr, "frame %d: ", i); recomp_arm7_state(stderr); }
        nds.RunFrame();
        if (getenv("FRAMEDIFF")) {
            static std::vector<u32> prev(256*384); static int changed = 0, total = 0, from = atoi(getenv("FRAMEDIFF"));
            void *t, *b;
            if (i >= from && nds.GPU.GetFramebuffers(&t, &b)) {
                std::vector<u32> cur(256*384);
                memcpy(cur.data(), t, 256*192*4); memcpy(cur.data()+256*192, b, 256*192*4);
                if (i > from) { total++; bool ch = cur != prev; if (ch) changed++; if (total <= 90) fputc(ch ? '#' : '.', stderr); if (total == 90) fputc('\n', stderr); }
                prev = cur;
                if (i == frames) fprintf(stderr, "frames %d-%d: picture changed on %d of %d frames\n", from, frames, changed, total);
            }
        }
        static int lastswap = -1;
        if (getenv("LOGSWAP") && (int)nds.GPU.ScreenSwap != lastswap) { lastswap = nds.GPU.ScreenSwap; fprintf(stderr, "frame %d: ScreenSwap=%d (engine A on %s)\n", i, lastswap, lastswap ? "top" : "bottom"); }
        if (i % every == 0) {
            void *t, *b;
            if (nds.GPU.GetFramebuffers(&t, &b)) {
                char p[512]; snprintf(p, sizeof p, "%s_%05d.ppm", argv[3], i);
                if (W) {
                    int WW = 256 + 2 * W;
                    FILE* f = fopen(p, "wb"); fprintf(f, "P6\n%d 384\n255\n", WW);
                    for (int scr = 0; scr < 2; scr++) {
                        u32 *L, *Rr; sr->GetWideMargins(scr, &L, &Rr);
                        u32* c = (u32*)(scr ? b : t);
                        std::vector<u32> fr(WW * 192);
                        for (int y = 0; y < 192; y++) for (int x = 0; x < WW; x++)
                            fr[y * WW + x] = x < W ? L[y * W + x] : x < W + 256 ? c[y * 256 + x - W] : Rr[y * W + x - W - 256];
                        for (int y = 0; y < 192; y++) for (int x = 0; x < WW; x++) {
                            u32 px = fr[y * WW + x];
                            unsigned char rgb[3] = {(unsigned char)(px >> 16), (unsigned char)(px >> 8), (unsigned char)px};
                            fwrite(rgb, 1, 3, f);
                        }
                    }
                    fclose(f);
                } else
                save_ppm(p, (u32*)t, (u32*)b);
            }
        }
    }
    if (getenv("PEEK")) { u32 a = strtoul(getenv("PEEK"), 0, 16); fprintf(stderr, "PEEK %08X = %08X\n", a, nds.ARM9Read32(a)); }
    if (getenv("DUMPITCM")) {
        const u8* it = nds.ARM9.ITCM; int nz = 0, first = -1, last = -1;
        for (int i = 0x4000; i < 0x8000; i++) if (it[i]) { nz++; if (first < 0) first = i; last = i; }
        fprintf(stderr, "ITCM 0x4000-0x8000 nonzero bytes=%d first=%X last=%X\n", nz, first, last);
    }
    if (getenv("DUMPBG")) {
        for (int e = 0; e < 2; e++) {
            GPU2D& g = e ? nds.GPU.GPU2D_B : nds.GPU.GPU2D_A;
            fprintf(stderr, "engine %c: DISPCNT=%08X swap=%d\n", 'A'+e, g.DispCnt, nds.GPU.ScreenSwap);
            for (int b = 0; b < 4; b++) fprintf(stderr, "  BG%d cnt=%04X x=%d y=%d\n", b, g.BGCnt[b], g.BGXPos[b], g.BGYPos[b]);
        }
        fprintf(stderr, "IO A 040-056:"); for (u32 a = 0x04000040; a < 0x04000056; a += 2) fprintf(stderr, " %04X", nds.ARM9Read16(a)); fprintf(stderr, "\n");
        { int vis = 0; for (int s2 = 0; s2 < 128; s2++) { u16 a0 = nds.ARM9Read16(0x07000000 + s2*8), a1 = nds.ARM9Read16(0x07000002 + s2*8), a2 = nds.ARM9Read16(0x07000004 + s2*8); if ((a0 & 0x300) == 0x200) continue; vis++; fprintf(stderr, "  OBJ%d a0=%04X a1=%04X a2=%04X\n", s2, a0, a1, a2); } }
        char q[512]; snprintf(q, sizeof q, "%s_vramA.bin", argv[3]);
        FILE* v = fopen(q, "wb"); for (u32 a = 0; a < 0x80000; a += 2) { u16 w = nds.ARM9Read16(0x06000000 + a); fwrite(&w, 2, 1, v); } fclose(v);
        snprintf(q, sizeof q, "%s_vramB.bin", argv[3]);
        v = fopen(q, "wb"); for (u32 a = 0; a < 0x20000; a += 2) { u16 w = nds.ARM9Read16(0x06200000 + a); fwrite(&w, 2, 1, v); } fclose(v);
    }
    char p[512]; snprintf(p, sizeof p, "%s_mainram.bin", argv[3]);
    FILE* o = fopen(p, "wb"); fwrite(nds.MainRAM, 1, 0x400000, o); fclose(o);
    fprintf(stderr, "ran %d frames\n", frames);
    if (getenv("RECOMP")) recomp_stats(stderr);
    return 0;
}
