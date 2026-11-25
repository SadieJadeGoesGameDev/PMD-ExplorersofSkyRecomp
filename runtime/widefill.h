// Sky Recomp widescreen: "picture screen" detection and blurred side fill.
// Works on a composed wide frame (FW = 256 + 2*WM pixels per row, 192 rows, 0x00RRGGBB / 0xFFRRGGBB).
// Some screens are 4:3 pictures (chapter title cards, intro artwork, story text on black, fades done with a
// full-screen layer, menus built only from 256-wide layers). Their margins hold leftover map data, blank tiles or
// an un-faded copy of the scene. We detect them per screen and fill the margins with a soft, dimmed, stretched blur
// of the picture itself, like a phone video on YouTube.
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#define WF_THRESH 0.7f
struct WideFillState { int bad_frames = 0, good_frames = 0; bool picture = false; };

static inline int wf_dist(uint32_t a, uint32_t b) {
    return abs((int)((a >> 16) & 255) - (int)((b >> 16) & 255)) + abs((int)((a >> 8) & 255) - (int)((b >> 8) & 255)) + abs((int)(a & 255) - (int)(b & 255));
}

// share of the most common colour in a strip (approximate: compares against a few candidates)
static inline float wf_flat(const uint32_t* fr, int FW, int x0, int w) {
    uint32_t cand[4] = {fr[x0] & 0xFFFFFF, fr[95 * FW + x0 + w / 2] & 0xFFFFFF, fr[191 * FW + x0 + w - 1] & 0xFFFFFF, fr[47 * FW + x0] & 0xFFFFFF};
    int best = 0;
    for (int c = 0; c < 4; c++) {
        int n = 0;
        for (int y = 0; y < 192; y += 2) for (int x = x0; x < x0 + w; x++) n += (fr[y * FW + x] & 0xFFFFFF) == cand[c];
        if (n > best) best = n;
    }
    return best / (float)(96 * w);
}

// 0..1: how much the margins look like they don't belong to the picture
static inline float wf_score(const uint32_t* fr, int WM) {
    const int FW = 256 + 2 * WM;
    float worst = 0;
    for (int side = 0; side < 2; side++) {
        int mx = side ? WM + 256 : WM - 1, ax = side ? WM + 255 : WM, a2x = side ? WM + 254 : WM + 1;
        int bad = 0, good = 0;
        for (int y = 0; y < 192; y++) {
            const uint32_t* r = fr + y * FW;
            int d1 = wf_dist(r[mx], r[ax]), d0 = wf_dist(r[ax], r[a2x]);
            if (d1 > 60 && d0 < 30) bad++;
            else if (d1 < 30) good++;
        }
        float s = bad / (float)(bad + good > 0 ? bad + good : 1);
        // outer part of the margin is one flat colour (blank map / backdrop) while the picture edge has detail
        int ow = WM < 12 ? WM : 12;
        float fo = wf_flat(fr, FW, side ? FW - ow : 0, ow);
        float fe = wf_flat(fr, FW, side ? WM + 256 - WM : WM, WM);
        if (fo > 0.9f && fe < 0.6f) s = s > 0.99f ? s : 0.99f;
        if (s > worst) worst = s;
    }
    return worst;
}

static inline bool wf_update(WideFillState& st, const uint32_t* fr, int WM) {
    bool bad = wf_score(fr, WM) > WF_THRESH;
    if (bad) { st.bad_frames++; st.good_frames = 0; } else { st.good_frames++; st.bad_frames = 0; }
    if (!st.picture && st.bad_frames >= 2) st.picture = true;
    if (st.picture && st.good_frames >= 8) st.picture = false;
    return st.picture;
}

// Fill both margins with a blurred, dimmed, enlarged copy of the 256x192 picture.
static inline void wf_fill(uint32_t* fr, int WM) {
    const int FW = 256 + 2 * WM, LW = 32, LH = 24;   // 8x8 boxes
    static float lo[LH][LW][3], tmp[LH][LW][3];
    for (int by = 0; by < LH; by++) for (int bx = 0; bx < LW; bx++) {
        float s[3] = {0, 0, 0};
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            uint32_t p = fr[(by * 8 + y) * FW + WM + bx * 8 + x];
            s[0] += (p >> 16) & 255; s[1] += (p >> 8) & 255; s[2] += p & 255;
        }
        for (int c = 0; c < 3; c++) lo[by][bx][c] = s[c] / 64.f;
    }
    for (int pass = 0; pass < 3; pass++) {              // 3x3 box blur, three passes (~gaussian)
        for (int y = 0; y < LH; y++) for (int x = 0; x < LW; x++) for (int c = 0; c < 3; c++) {
            float s = 0;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                int yy = y + dy < 0 ? 0 : y + dy >= LH ? LH - 1 : y + dy, xx = x + dx < 0 ? 0 : x + dx >= LW ? LW - 1 : x + dx;
                s += lo[yy][xx][c];
            }
            tmp[y][x][c] = s / 9.f;
        }
        memcpy(lo, tmp, sizeof lo);
    }
    // the blurred picture is scaled up so it covers the whole wide frame (zoom = FW/256), then dimmed
    // dim busy pictures so the margins sit back; flat screens (white flashes, fades, plain backgrounds) keep their colour
    float mean = 0, var = 0;
    for (int y = 0; y < LH; y++) for (int x = 0; x < LW; x++) mean += lo[y][x][0] + lo[y][x][1] + lo[y][x][2];
    mean /= LH * LW;
    for (int y = 0; y < LH; y++) for (int x = 0; x < LW; x++) { float d = lo[y][x][0] + lo[y][x][1] + lo[y][x][2] - mean; var += d * d; }
    float sd = sqrtf(var / (LH * LW)) / 3.f;
    const float zoom = FW / 256.f, dim = sd < 10.f ? 1.f : sd < 25.f ? 1.f - 0.3f * (sd - 10.f) / 15.f : 0.7f;
    for (int y = 0; y < 192; y++) {
        float sy = ((y - 96) / zoom + 96) / 8.f - 0.5f;
        int y0 = (int)sy; float fy = sy - y0; if (y0 < 0) { y0 = 0; fy = 0; } if (y0 >= LH - 1) { y0 = LH - 2; fy = 1; }
        uint32_t* row = fr + y * FW;
        for (int x = 0; x < FW; x++) {
            if (x == WM) x = WM + 256;
            float sx = ((x - FW / 2.f) / zoom + 128) / 8.f - 0.5f;
            int x0 = (int)sx; float fx = sx - x0; if (sx < 0) { x0 = 0; fx = 0; } if (x0 >= LW - 1) { x0 = LW - 2; fx = 1; }
            uint32_t out = 0xFF000000;
            for (int c = 0; c < 3; c++) {
                float v = (lo[y0][x0][c] * (1 - fx) + lo[y0][x0 + 1][c] * fx) * (1 - fy) + (lo[y0 + 1][x0][c] * (1 - fx) + lo[y0 + 1][x0 + 1][c] * fx) * fy;
                int iv = (int)(v * dim + 0.5f); if (iv > 255) iv = 255;
                out |= (uint32_t)iv << (16 - 8 * c);
            }
            row[x] = out;
        }
    }
}
