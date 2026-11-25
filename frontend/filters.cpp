// Sky Recomp display filters (CPU, 4x output).
//  - xBR (4xBR, Hyllian's edge-directed pixel-art scaler): smooths diagonal and curved edges, keeps art crisp.
//  - CRT: scanlines + soft horizontal beam + faint aperture grille.
// Input/output pixels are 0xAARRGGBB. dst is (4w) x (4h).
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <thread>

namespace filters {

// ---------------------------------------------------------------- xBR
static inline int yuv_of(uint32_t c) {
    int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    int y = (r * 299 + g * 587 + b * 114) / 1000;
    int u = ((b - y) * 492) / 1000 + 128;
    int v = ((r - y) * 877) / 1000 + 128;
    return (y << 16) | ((u & 0xFF) << 8) | (v & 0xFF);
}
static inline int df(int a, int b) {
    return std::abs(((a >> 16) & 0xFF) - ((b >> 16) & 0xFF)) * 48 + std::abs(((a >> 8) & 0xFF) - ((b >> 8) & 0xFF)) * 7 +
           std::abs((a & 0xFF) - (b & 0xFF)) * 6;
}
static inline bool eq(int a, int b) { return df(a, b) < 155; }
static inline uint32_t blend(uint32_t dst, uint32_t src, int a) {   // a/256 of src
    uint32_t r = (((dst >> 16) & 0xFF) * (256 - a) + ((src >> 16) & 0xFF) * a) >> 8;
    uint32_t g = (((dst >> 8) & 0xFF) * (256 - a) + ((src >> 8) & 0xFF) * a) >> 8;
    uint32_t b = ((dst & 0xFF) * (256 - a) + (src & 0xFF) * a) >> 8;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

// neighbourhood offsets for the bottom-right corner; other corners are 90-degree rotations of it
enum { PA, PB, PC, PD, PE, PF, PG, PH, PI, A1, B1, C1, A0, D0, G0, C4, F4, I4, G5, H5, I5, NP };
static const int base_off[NP][2] = {
    {-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {0, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1},
    {-1, -2}, {0, -2}, {1, -2}, {-2, -1}, {-2, 0}, {-2, 1}, {2, -1}, {2, 0}, {2, 1}, {-1, 2}, {0, 2}, {1, 2}};
// output cells (x,y in the 4x4 block) used by the corner macros, bottom-right orientation
enum { N15, N14, N11, N3, N7, N10, N13, N12, NN };
static const int base_cell[NN][2] = {{3, 3}, {2, 3}, {3, 2}, {3, 0}, {3, 1}, {2, 2}, {1, 3}, {0, 3}};
static int rot_off[4][NP][2], rot_cell[4][NN];
static bool tables_ready = false;
static void init_tables() {
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < NP; i++) {
            int x = base_off[i][0], y = base_off[i][1];
            for (int r = 0; r < k; r++) { int t = x; x = -y; y = t; }     // rotate 90 degrees
            rot_off[k][i][0] = x; rot_off[k][i][1] = y;
        }
        for (int i = 0; i < NN; i++) {
            // rotate cell centre (x-1.5, y-1.5) in doubled coordinates
            int x = base_cell[i][0] * 2 - 3, y = base_cell[i][1] * 2 - 3;
            for (int r = 0; r < k; r++) { int t = x; x = -y; y = t; }
            rot_cell[k][i] = ((y + 3) / 2) * 4 + (x + 3) / 2;
        }
    }
    tables_ready = true;
}

static void xbr_rows(const uint32_t *src, const int *yuv, int w, int h, uint32_t *dst, int y0, int y1) {
    const int dw = w * 4;
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < w; x++) {
            uint32_t E[16];
            uint32_t ce = src[y * w + x];
            for (int i = 0; i < 16; i++) E[i] = ce;
            for (int k = 0; k < 4; k++) {
                uint32_t P[NP]; int Y[NP];
                for (int i = 0; i < NP; i++) {
                    int xx = x + rot_off[k][i][0], yy = y + rot_off[k][i][1];
                    xx = xx < 0 ? 0 : xx >= w ? w - 1 : xx;
                    yy = yy < 0 ? 0 : yy >= h ? h - 1 : yy;
                    P[i] = src[yy * w + xx]; Y[i] = yuv[yy * w + xx];
                }
                const int *c = rot_cell[k];
                if (P[PE] == P[PH] || P[PE] == P[PF]) continue;
                int e = df(Y[PE], Y[PC]) + df(Y[PE], Y[PG]) + df(Y[PI], Y[H5]) + df(Y[PI], Y[F4]) + (df(Y[PH], Y[PF]) << 2);
                int i = df(Y[PH], Y[PD]) + df(Y[PH], Y[I5]) + df(Y[PF], Y[I4]) + df(Y[PF], Y[PB]) + (df(Y[PE], Y[PI]) << 2);
                uint32_t px = df(Y[PE], Y[PF]) <= df(Y[PE], Y[PH]) ? P[PF] : P[PH];
                if (e < i && ((!eq(Y[PF], Y[PB]) && !eq(Y[PH], Y[PD])) ||
                              (eq(Y[PE], Y[PI]) && (!eq(Y[PF], Y[I4]) && !eq(Y[PH], Y[I5]))) ||
                              eq(Y[PE], Y[PG]) || eq(Y[PE], Y[PC]))) {
                    int ke = df(Y[PF], Y[PG]), ki = df(Y[PH], Y[PC]);
                    bool ex2 = P[PE] != P[PC] && P[PB] != P[PC];
                    bool ex3 = P[PE] != P[PG] && P[PD] != P[PG];
                    if ((ke << 1) <= ki && ex3 && ke >= (ki << 1) && ex2) {          // both
                        E[c[N13]] = blend(E[c[N13]], px, 192);
                        E[c[N12]] = blend(E[c[N12]], px, 64);
                        E[c[N15]] = E[c[N14]] = E[c[N11]] = px;
                        E[c[N10]] = E[c[N3]] = E[c[N12]];
                        E[c[N7]] = E[c[N13]];
                    } else if ((ke << 1) <= ki && ex3) {                             // shallow
                        E[c[N11]] = blend(E[c[N11]], px, 192);
                        E[c[N13]] = blend(E[c[N13]], px, 192);
                        E[c[N10]] = blend(E[c[N10]], px, 64);
                        E[c[N12]] = blend(E[c[N12]], px, 64);
                        E[c[N14]] = px; E[c[N15]] = px;
                    } else if (ke >= (ki << 1) && ex2) {                             // steep
                        E[c[N14]] = blend(E[c[N14]], px, 192);
                        E[c[N7]] = blend(E[c[N7]], px, 192);
                        E[c[N10]] = blend(E[c[N10]], px, 64);
                        E[c[N3]] = blend(E[c[N3]], px, 64);
                        E[c[N11]] = px; E[c[N15]] = px;
                    } else {                                                         // 45 degrees
                        E[c[N11]] = blend(E[c[N11]], px, 128);
                        E[c[N14]] = blend(E[c[N14]], px, 128);
                        E[c[N15]] = px;
                    }
                } else if (e <= i) {
                    E[c[N15]] = blend(E[c[N15]], px, 128);
                }
            }
            uint32_t *o = dst + (y * 4) * dw + x * 4;
            for (int r = 0; r < 4; r++) memcpy(o + r * dw, E + r * 4, 16);
        }
}

void xbr4(const uint32_t *src, int w, int h, uint32_t *dst) {
    if (!tables_ready) init_tables();
    static std::vector<int> yuv;
    yuv.resize((size_t)w * h);
    for (int i = 0; i < w * h; i++) yuv[i] = yuv_of(src[i]);
    const int T = 4;
    std::thread th[T];
    for (int t = 0; t < T; t++)
        th[t] = std::thread(xbr_rows, src, yuv.data(), w, h, dst, h * t / T, h * (t + 1) / T);
    for (auto &t : th) t.join();
}

// ---------------------------------------------------------------- CRT (horizontal scanlines)
// Rendered straight at the output height so every scanline lands on whole screen pixels (no moire).
// Each source line is a soft gaussian beam; bright lines bloom wider, like a real tube. 4x horizontally with a
// sharpened ramp between pixels, then the GPU stretches it to the window width.
static float g_beam[64][33];        // [distance from line centre in 1/32 line][brightness 0..32]
static bool g_beam_ready = false;
static void init_beam() {
    for (int d = 0; d < 64; d++)
        for (int l = 0; l <= 32; l++) {
            float dist = d / 32.0f;                              // 0..2 lines
            float sigma = 0.30f + 0.12f * (l / 32.0f);            // brighter = wider beam
            g_beam[d][l] = __builtin_expf(-dist * dist / (2 * sigma * sigma)) / (sigma * 2.5066283f);   // area 1: same average brightness as Off
        }
    g_beam_ready = true;
}
// horizontally expanded source (4x, sharpened ramp), computed once per frame
static std::vector<uint32_t> g_hx;
static inline int sharp_t(float pos, float scale, int *i0);
static void crt_expand(const uint32_t *src, int w, int dw, int y0, int y1) {
    const float scale = dw / (float)w;
    for (int y = y0; y < y1; y++) {
        const uint32_t *row = src + y * w;
        uint32_t *o = g_hx.data() + (size_t)y * dw;
        for (int sx = 0; sx < dw; sx++) {
            int x0; int t = sharp_t((sx + 0.5f) * w / dw, scale, &x0);
            uint32_t a = row[x0 < 0 ? 0 : x0], b = row[x0 + 1 >= w ? w - 1 : x0 + 1];
            uint32_t r = (((a >> 16) & 0xFF) * (256 - t) + ((b >> 16) & 0xFF) * t) >> 8;
            uint32_t g = (((a >> 8) & 0xFF) * (256 - t) + ((b >> 8) & 0xFF) * t) >> 8;
            uint32_t bl = ((a & 0xFF) * (256 - t) + (b & 0xFF) * t) >> 8;
            uint32_t lum = (r * 3 + g * 6 + bl) / 10;
            o[sx] = (lum >> 3) << 24 | r << 16 | g << 8 | bl;
        }
    }
}
static void crt_rows(int dw, int h, uint32_t *dst, int dh, int y0, int y1) {
    for (int y = y0; y < y1; y++) {
        float sy = (y + 0.5f) * h / dh - 0.5f;                     // position in source lines
        int i0 = (int)__builtin_floorf(sy);
        int d0 = (int)((sy - i0) * 32.0f + 0.5f), d1 = 32 - d0;
        const uint32_t *A = g_hx.data() + (size_t)(i0 < 0 ? 0 : i0) * dw;
        const uint32_t *B = g_hx.data() + (size_t)(i0 + 1 >= h ? h - 1 : i0 + 1) * dw;
        bool hasA = i0 >= 0, hasB = i0 + 1 < h;
        uint32_t *o = dst + (size_t)y * dw;
        for (int sx = 0; sx < dw; sx++) {
            int R = 0, G = 0, Bc = 0;
            if (hasA) { uint32_t p = A[sx]; int wg = (int)(g_beam[d0][p >> 24] * 1024.0f); R += ((p >> 16) & 0xFF) * wg; G += ((p >> 8) & 0xFF) * wg; Bc += (p & 0xFF) * wg; }
            if (hasB) { uint32_t p = B[sx]; int wg = (int)(g_beam[d1][p >> 24] * 1024.0f); R += ((p >> 16) & 0xFF) * wg; G += ((p >> 8) & 0xFF) * wg; Bc += (p & 0xFF) * wg; }
            R >>= 10; G >>= 10; Bc >>= 10;
            if (R > 255) R = 255; if (G > 255) G = 255; if (Bc > 255) Bc = 255;
            o[sx] = 0xFF000000u | (R << 16) | (G << 8) | Bc;
        }
    }
}

void crt(const uint32_t *src, int w, int h, uint32_t *dst, int dh, int dw) {
    if (!g_beam_ready) init_beam();
    g_hx.resize((size_t)dw * h);
    const int T = 4;
    std::thread th[T];
    for (int t = 0; t < T; t++) th[t] = std::thread(crt_expand, src, w, dw, h * t / T, h * (t + 1) / T);
    for (auto &t : th) t.join();
    for (int t = 0; t < T; t++) th[t] = std::thread(crt_rows, dw, h, dst, dh, dh * t / T, dh * (t + 1) / T);
    for (auto &t : th) t.join();
}

// ---------------------------------------------------------------- Smooth (sharp bilinear)
// Each source pixel becomes a crisp block, but the one output pixel that straddles two blocks is blended, so pixels
// stay square at any window size without shimmering or uneven widths. Output is 4x wide and dh tall.
static inline uint32_t lerp4(uint32_t a, uint32_t b, uint32_t c, uint32_t d, int tx, int ty) {   // t in 0..256
    uint32_t o = 0xFF000000u;
    for (int sh = 0; sh <= 16; sh += 8) {
        int top = ((a >> sh) & 255) * (256 - tx) + ((b >> sh) & 255) * tx;
        int bot = ((c >> sh) & 255) * (256 - tx) + ((d >> sh) & 255) * tx;
        o |= (uint32_t)((top * (256 - ty) + bot * ty) >> 16) << sh;
    }
    return o;
}
static inline int sharp_t(float pos, float scale, int *i0) {          // pos in source pixels (centre at .5)
    float p = pos - 0.5f; int i = (int)__builtin_floorf(p); float f = p - i;
    f = (f - 0.5f) * scale + 0.5f; f = f < 0 ? 0 : f > 1 ? 1 : f;
    *i0 = i; return (int)(f * 256.0f + 0.5f);
}
static void smooth_rows(const uint32_t *src, int w, int h, uint32_t *dst, int dh, int dw, int y0, int y1, int grid) {
    const float sy_scale = dh / (float)h, sx_scale = dw / (float)w;
    for (int y = y0; y < y1; y++) {
        int iy; float py = (y + 0.5f) * h / dh;
        int ty = sharp_t(py, sy_scale, &iy);
        int ra = iy < 0 ? 0 : iy, rb = iy + 1 >= h ? h - 1 : iy + 1;
        const uint32_t *A = src + (size_t)ra * w, *B = src + (size_t)rb * w;
        // DS Screen: faint gaps between pixel rows/columns (only when each pixel is big enough to show them)
        float fy = py - (int)py; int rowdark = 0;
        if (grid && sy_scale >= 3.0f) { float e = fy < 0.5f ? fy : 1 - fy; float edge = 0.5f / sy_scale; if (e < edge) rowdark = 30; }
        uint32_t *o = dst + (size_t)y * dw;
        for (int x = 0; x < dw; x++) {
            int ix; float px = (x + 0.5f) * w / dw;
            int tx = sharp_t(px, sx_scale, &ix);
            int ca = ix < 0 ? 0 : ix, cb = ix + 1 >= w ? w - 1 : ix + 1;
            uint32_t c = lerp4(A[ca], A[cb], B[ca], B[cb], tx, ty);
            if (grid) {
                float fx = px - (int)px; int coldark = sx_scale >= 3.0f && (fx < 0.5f ? fx : 1 - fx) < 0.5f / sx_scale ? 22 : 0;
                int k = 256 - rowdark - coldark;
                if (k < 256) {
                    uint32_t r = ((c >> 16) & 255) * k >> 8, g = ((c >> 8) & 255) * k >> 8, b = (c & 255) * k >> 8;
                    c = 0xFF000000u | r << 16 | g << 8 | b;
                }
            }
            o[x] = c;
        }
    }
}
void smooth(const uint32_t *src, int w, int h, uint32_t *dst, int dh, int dw, int grid) {
    const int T = 4; std::thread th[T];
    for (int t = 0; t < T; t++) th[t] = std::thread(smooth_rows, src, w, h, dst, dh, dw, dh * t / T, dh * (t + 1) / T, grid);
    for (auto &t : th) t.join();
}

// ---------------------------------------------------------------- Glow (scanlines + bloom)
// The scanline image plus a soft halo around bright things, like light bleeding on a tube. Flat screen.
static std::vector<float> g_bloom;
static void bloom_rows(int w, int h, uint32_t *dst, int dh, int dw, int y0, int y1) {
    for (int y = y0; y < y1; y++) {
        float sy = (y + 0.5f) * h / dh - 0.5f; int iy = (int)__builtin_floorf(sy); float fy = sy - iy;
        int ya = iy < 0 ? 0 : iy, yb = iy + 1 >= h ? h - 1 : iy + 1;
        uint32_t *o = dst + (size_t)y * dw;
        for (int x = 0; x < dw; x++) {
            float sx = (x + 0.5f) * w / dw - 0.5f; int ix = (int)__builtin_floorf(sx); float fx = sx - ix;
            int xa = ix < 0 ? 0 : ix, xb = ix + 1 >= w ? w - 1 : ix + 1;
            uint32_t c = o[x]; uint32_t out = 0xFF000000u;
            for (int ch = 0; ch < 3; ch++) {
                const float *B = g_bloom.data() + ch;
                float v = (B[(ya * w + xa) * 3] * (1 - fx) + B[(ya * w + xb) * 3] * fx) * (1 - fy) + (B[(yb * w + xa) * 3] * (1 - fx) + B[(yb * w + xb) * 3] * fx) * fy;
                int sh = 16 - 8 * ch; int base = (c >> sh) & 255;
                int r = (int)(base * 0.75f + v * 0.25f + 0.5f);   // light spreads out of the lines, total stays the same
                out |= (uint32_t)(r > 255 ? 255 : r) << sh;
            }
            o[x] = out;
        }
    }
}
void glow(const uint32_t *src, int w, int h, uint32_t *dst, int dh, int dw) {
    crt(src, w, h, dst, dh, dw);
    // a soft separable blur of the picture at source resolution (the halo)
    g_bloom.assign((size_t)w * h * 3, 0.0f);
    std::vector<float> tmp((size_t)w * h * 3);
    for (int i = 0; i < w * h; i++) {
        uint32_t p = src[i]; float r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
        float k = 1.0f;
        tmp[i * 3] = r * k; tmp[i * 3 + 1] = g * k; tmp[i * 3 + 2] = b * k;
    }
    const int R = 5; float wt[R + 1], sum = 0;
    for (int i = 0; i <= R; i++) { wt[i] = __builtin_expf(-i * i / (2.0f * 1.8f * 1.8f)); sum += i ? 2 * wt[i] : wt[i]; }
    for (int i = 0; i <= R; i++) wt[i] /= sum;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < 3; c++) {
        float v = 0; for (int d = -R; d <= R; d++) { int xx = x + d < 0 ? 0 : x + d >= w ? w - 1 : x + d; v += tmp[(y * w + xx) * 3 + c] * wt[d < 0 ? -d : d]; }
        g_bloom[(y * w + x) * 3 + c] = v;
    }
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < 3; c++) {
        float v = 0; for (int d = -R; d <= R; d++) { int yy = y + d < 0 ? 0 : y + d >= h ? h - 1 : y + d; v += g_bloom[(yy * w + x) * 3 + c] * wt[d < 0 ? -d : d]; }
        tmp[(y * w + x) * 3 + c] = v;
    }
    g_bloom.swap(tmp);
    const int T = 4; std::thread th[T];
    for (int t = 0; t < T; t++) th[t] = std::thread(bloom_rows, w, h, dst, dh, dw, dh * t / T, dh * (t + 1) / T);
    for (auto &t : th) t.join();
}

}  // namespace filters
