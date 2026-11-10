/*
 * DOC — Rosto "Mochi" (olhos + boca) em C puro, sem dependência do LVGL.
 * Arquivo: main/display/doc_face.c
 *
 * Porta do sketch Arduino "DOC AI - Mochi face". As coordenadas das
 * expressões continuam no espaço virtual 240x280 do sketch. Toda primitiva
 * (retângulo, círculo, triângulo, linha) passa por uma transformação
 * virtual -> tela (g_s, g_ox, g_oy). Interpolando essa transformação entre
 * "rosto inteiro" e "faixa superior" os olhos sobem suavemente.
 *
 * O efeito de "matriz de LED" e o logo de abertura do sketch não foram
 * portados, e o sensor de toque continua com a função que ele já tem no
 * firmware.
 */

#include "doc_face.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * DOC — 1. AJUSTES FÁCEIS DE ALTERAR
 * ============================================================ */

/* Espaço virtual das expressões (igual ao canvas do sketch). */
#define VW 240.0f
#define VH 280.0f

/* Rosto inteiro: o espaço virtual cabe na tela mantendo a proporção. */
#define S_FULL  (128.0f / VW)
#define OX_FULL 0.0f
#define OY_FULL ((160.0f - VH * S_FULL) / 2.0f)

/*
 * Faixa superior: o centro dos olhos (120,130) vai para (64,16), o meio da
 * faixa de 32 px. Escala 0.40 deixa o olho de 75 unidades com 30 px.
 */
#define S_BAND  0.40f
#define OX_BAND (64.0f - 120.0f * S_BAND)
#define OY_BAND (16.0f - 130.0f * S_BAND)

/* Duração da subida/descida dos olhos e da boca -> contorno, em segundos. */
#define APP_ANIM_SECONDS 0.35f

/* Boca padrão (expressão ALIVE) em unidades virtuais: ponto de partida do contorno. */
#define MOUTH_X 105.0f
#define MOUTH_Y 179.0f
#define MOUTH_W 30.0f
#define MOUTH_H 8.0f

/* Contorno final: área do app. */
#define FRAME_RADIUS 6
#define FRAME_BORDER 2

/* ============================================================
 * DOC — 2. ESTADO
 * ============================================================ */

static uint16_t g_fg = 0xFFFF;   /* cor do rosto */
static uint16_t g_bg = 0x0000;   /* cor do fundo */

static uint16_t *g_fb = NULL;
static int g_stride = DOC_FACE_W;
static int g_rows = DOC_FACE_H;

static float g_s = S_FULL, g_ox = OX_FULL, g_oy = OY_FULL;
static uint16_t g_col = 0xFFFF;   /* ffColor do sketch */
static bool g_mouth_on = true;

static uint32_t g_now = 0;
static uint32_t g_last_tick = 0;
static bool g_started = false;

static doc_emotion_t g_emo = DOC_EMO_ALIVE;

static float cur_eye_ox, cur_eye_oy, tgt_eye_ox, tgt_eye_oy;
static int eye_ox, eye_oy;
static uint32_t next_look_time, look_reset_time;

static bool is_blinking;
static uint32_t blink_start, blink_dur, next_blink_time;
static float blink_factor = 1.0f;

static bool is_yawning;
static uint32_t next_yawn_time, yawn_end_time;
static int mouth_extra_h;

static bool g_talking;
static bool g_bias_on;
static float g_bias_x, g_bias_y;

static bool g_app_mode;
static float g_app_p;          /* 0..1 */
static bool g_band_settled;

static float spiral_angle;
static float tear_progress;

static uint32_t g_rng = 0x2545F491u;

/* ============================================================
 * DOC — 3. UTILIDADES
 * ============================================================ */

/* Equivalente ao random(lo, hi) do Arduino: valor em [lo, hi). */
static int32_t rnd(int32_t lo, int32_t hi)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    if (hi <= lo) return lo;
    return lo + (int32_t)((g_rng >> 8) % (uint32_t)(hi - lo));
}

static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int iround(float v) { return (int)floorf(v + 0.5f); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float smooth01(float p) { return p * p * (3.0f - 2.0f * p); }

/* ============================================================
 * DOC — 4. PRIMITIVAS (espaço virtual -> pixels da tela)
 * ============================================================ */

static inline float sx(float x) { return g_ox + x * g_s; }
static inline float sy(float y) { return g_oy + y * g_s; }

static inline void plot(int x, int y, uint16_t c)
{
    if (x >= 0 && x < DOC_FACE_W && y >= 0 && y < g_rows) {
        g_fb[y * g_stride + x] = c;
    }
}

/* Preenche [x0,x1) x [y0,y1) em pixels de tela, já recortado. */
static void fill_rect_s(int x0, int y0, int x1, int y1, uint16_t c)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > DOC_FACE_W) x1 = DOC_FACE_W;
    if (y1 > g_rows) y1 = g_rows;
    for (int y = y0; y < y1; y++) {
        uint16_t *row = g_fb + y * g_stride;
        for (int x = x0; x < x1; x++) row[x] = c;
    }
}

static void v_rect(float x, float y, float w, float h, uint16_t c)
{
    if (w <= 0 || h <= 0) return;
    int x0 = iround(sx(x)), x1 = iround(sx(x + w));
    int y0 = iround(sy(y)), y1 = iround(sy(y + h));
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    fill_rect_s(x0, y0, x1, y1, c);
}

static void v_rrect(float x, float y, float w, float h, float r, uint16_t c)
{
    if (w <= 0 || h <= 0) return;
    int x0 = iround(sx(x)), x1 = iround(sx(x + w));
    int y0 = iround(sy(y)), y1 = iround(sy(y + h));
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;

    float rs = r * g_s;
    float maxr = 0.5f * (float)imin(x1 - x0, y1 - y0);
    if (rs > maxr) rs = maxr;
    if (rs < 1.0f) {
        fill_rect_s(x0, y0, x1, y1, c);
        return;
    }

    for (int yy = y0; yy < y1; yy++) {
        float top = ((float)yy + 0.5f) - (float)y0;
        float bot = (float)y1 - ((float)yy + 0.5f);
        float d = top < bot ? top : bot;
        int inset = 0;
        if (d < rs) {
            float k = rs - d;
            inset = iround(rs - sqrtf(fmaxf(rs * rs - k * k, 0.0f)));
        }
        fill_rect_s(x0 + inset, yy, x1 - inset, yy + 1, c);
    }
}

static void v_circle(float cx, float cy, float r, uint16_t c)
{
    float X = sx(cx), Y = sy(cy);
    float R = (r + 0.5f) * g_s;
    if (R < 0.85f) R = 0.85f;
    int xa = (int)floorf(X - R - 1.0f), xb = (int)ceilf(X + R + 1.0f);
    int ya = (int)floorf(Y - R - 1.0f), yb = (int)ceilf(Y + R + 1.0f);
    for (int y = ya; y <= yb; y++) {
        for (int x = xa; x <= xb; x++) {
            float dx = (float)x + 0.5f - X, dy = (float)y + 0.5f - Y;
            if (dx * dx + dy * dy <= R * R) plot(x, y, c);
        }
    }
}

static void v_circle_outline(float cx, float cy, float r, uint16_t c)
{
    float X = sx(cx), Y = sy(cy);
    float R = r * g_s;
    if (R < 1.5f) R = 1.5f;
    int steps = (int)(R * 8.0f) + 8;
    for (int i = 0; i < steps; i++) {
        float a = 6.2831853f * (float)i / (float)steps;
        plot(iround(X + cosf(a) * R), iround(Y + sinf(a) * R), c);
    }
}

static void v_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint16_t c)
{
    float ax = sx(x0), ay = sy(y0), bx = sx(x1), by = sy(y1), cx = sx(x2), cy = sy(y2);
    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (fabsf(area) < 0.01f) return;

    int xa = (int)floorf(fminf(ax, fminf(bx, cx))), xb = (int)ceilf(fmaxf(ax, fmaxf(bx, cx)));
    int ya = (int)floorf(fminf(ay, fminf(by, cy))), yb = (int)ceilf(fmaxf(ay, fmaxf(by, cy)));
    for (int y = ya; y <= yb; y++) {
        for (int x = xa; x <= xb; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
            float w1 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
            float w2 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
            if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)) {
                plot(x, y, c);
            }
        }
    }
}

static void v_line(float x0f, float y0f, float x1f, float y1f, uint16_t c)
{
    int x0 = iround(sx(x0f)), y0 = iround(sy(y0f));
    int x1 = iround(sx(x1f)), y1 = iround(sy(y1f));
    int dx = abs(x1 - x0), sxs = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sys = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        plot(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sxs; }
        if (e2 <= dx) { err += dx; y0 += sys; }
    }
}

static void v_px(float x, float y, uint16_t c)
{
    plot(iround(sx(x)), iround(sy(y)), c);
}

/* Nomes equivalentes aos do sketch (ffOn/ffOff, ffBox...). */
#define ON  g_fg
#define OFF g_bg

static void ffOn(void)  { g_col = ON; }
static void ffOff(void) { g_col = OFF; }
static void ffBox(int x, int y, int w, int h) { v_rect(x, y, w, h, g_col); }
static void ffRBox(int x, int y, int w, int h, int r)
{
    r = imin(r, imin(w, h) / 2 - 1);
    if (r < 1) { ffBox(x, y, w, h); return; }
    v_rrect(x, y, w, h, r, g_col);
}
static void ffTri(int x0, int y0, int x1, int y1, int x2, int y2) { v_tri(x0, y0, x1, y1, x2, y2, g_col); }
static void ffLine(int x0, int y0, int x1, int y1) { v_line(x0, y0, x1, y1, g_col); }
static void ffThick(int x0, int y0, int x1, int y1)
{
    ffLine(x0, y0, x1, y1);         ffLine(x0 + 1, y0, x1 + 1, y1);
    ffLine(x0, y0 + 1, x1, y1 + 1); ffLine(x0 + 1, y0 + 1, x1 + 1, y1 + 1);
}
static void ffDisc(int x, int y, int r) { v_circle(x, y, r, g_col); }
static void ffCapsule(int x0, int y0, int x1, int y1, int r)
{
    int n = imax(imax(abs(x1 - x0), abs(y1 - y0)), 1);
    for (int i = 0; i <= n; i++) ffDisc(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, r);
}
static float ffEase(float p) { return 0.5f - 0.5f * cosf(p * 3.14159f); }

/* ============================================================
 * DOC — 5. OLHOS E ENFEITES BÁSICOS
 * ============================================================ */

/* Olho arredondado seguro (mesma regra do sketch). */
static void drawEye(int x, int y, int w, int h, int r)
{
    if (y + h <= 0 || y >= (int)VH) return;
    if (x + w <= 0 || x >= (int)VW) return;

    if (x < 0) { w += x; x = 0; if (w <= 0) return; v_rect(x, y, w, h, ON); return; }
    if (x + w > (int)VW) { w = (int)VW - x; if (w <= 0) return; v_rect(x, y, w, h, ON); return; }

    int safe_r = imin(r, imin(w / 2, h / 2));
    if (h <= 5 || w <= 5 || safe_r <= 1) v_rect(x, y, w, h, ON);
    else v_rrect(x, y, w, h, safe_r, ON);
}

static void drawSpiral(int cx, int cy, int direction, float rotation)
{
    float angle = 0, radius = 0;
    while (radius < 13) {
        float a = (angle + rotation) * (float)direction;
        int x = cx + (int)(cosf(a) * radius);
        int y = cy + (int)(sinf(a) * radius);
        v_px(x, y, ON);
        v_px(x + 1, y, ON);
        angle += 0.4f; radius += 0.28f;
    }
}

static void drawHeart(int x, int y, float scale)
{
    int r = (int)(6 * scale), off_x = (int)(6 * scale), off_y = (int)(4 * scale), tri_h = (int)(12 * scale);
    v_circle(x - off_x, y - off_y, r, ON);
    v_circle(x + off_x, y - off_y, r, ON);
    v_tri(x - r * 2, y - off_y, x + r * 2, y - off_y, x, y + tri_h, ON);
}

static void drawSmileArc(int cx, int cy, int R)
{
    for (int i = 0; i <= 16; i++) {
        float a = 3.14159265f * (0.1f + 0.8f * (float)i / 16.0f);
        v_circle(cx + (int)(cosf(a) * R), cy + (int)(sinf(a) * R), 1, ON);
    }
}

static void drawFloatingHearts(void)
{
    static const int heart_x[3] = {26, 120, 214};
    for (int i = 0; i < 3; i++) {
        float prog = (float)((g_now + (uint32_t)i * 900u) % 2700u) / 2700.0f;
        int hy = 284 - (int)(prog * 290);
        int hx = heart_x[i] + (int)(sinf(prog * 6.2832f + (float)i) * 3.0f);
        if (hy >= 4 && hy <= 275) drawHeart(hx, hy, 0.4f);
    }
}

static void drawTears(int left_x, int right_x, int start_y)
{
    for (int i = 0; i < 4; i++) {
        int drop_y = start_y + ((int)(tear_progress + i * 7) % 22);
        v_circle(left_x, drop_y, 1, ON);
        v_circle(right_x, drop_y, 1, ON);
    }
    tear_progress += 0.5f;
}

static void drawAngryVein(int cx, int cy)
{
    int pulse = (int)((g_now / 150) % 2);
    int r = pulse == 0 ? 6 : 8;
    int elbow = pulse == 0 ? 3 : 4;
    static const int signs[4][2] = {{1, -1}, {-1, -1}, {-1, 1}, {1, 1}};
    for (int i = 0; i < 4; i++) {
        int s_x = signs[i][0], s_y = signs[i][1];
        v_line(cx + s_x * 2, cy + s_y * r, cx + s_x * elbow, cy + s_y * elbow, ON);
        v_line(cx + s_x * 3, cy + s_y * r, cx + s_x * (elbow + 1), cy + s_y * (elbow + 1), ON);
        v_line(cx + s_x * elbow, cy + s_y * elbow, cx + s_x * r, cy + s_y * 2, ON);
        v_line(cx + s_x * (elbow + 1), cy + s_y * (elbow + 1), cx + s_x * r, cy + s_y * 3, ON);
    }
}

static void drawZ(int x, int y, int s)
{
    v_line(x, y, x + s, y, ON);
    v_line(x + s, y, x, y + s, ON);
    v_line(x, y + s, x + s, y + s, ON);
}

/* ============================================================
 * DOC — 6. EXPRESSÕES "MOCHI" (boca só aparece com g_mouth_on)
 * ============================================================ */

#define FF_W 240
#define FF_H 280

static void ffMochiEye(int cx, int cy, int ew, int hh)
{
    ffOn();
    ffRBox(cx - ew / 2, cy - hh / 2, ew, hh, (int)(imin(ew, hh) * 0.45f));
}

static void ffSmirk(int mx, int my, int mw, int mh)
{
    int a = mw / 2, r = imax(1, mh / 2);
    ffOn();
    ffCapsule(mx - a, my, mx + a - 2, my - 2, r);
    ffCapsule(mx + a - 2, my - 2, mx + a + 1, my - 5, r);
}

static void ffArcSmile(int cx, int cy, int R)
{
    ffOn();
    for (int i = 0; i <= 16; i++) {
        float a = 3.14159265f * (0.1f + 0.8f * (float)i / 16.0f);
        ffDisc(cx + (int)(cosf(a) * R), cy + (int)(sinf(a) * R), 1);
    }
}

static void ffSmile(int lx, int ly, int rx, int ry, int ew, int eh, int mx, int my, float bf)
{
    int dy = (int)(sinf(g_now * 0.003f) * 1.5f);
    int hh = imax(3, (int)(eh * 0.9f * bf));
    ffMochiEye(lx, ly + dy, ew, hh);
    ffMochiEye(rx, ry + dy, ew, hh);
    if (g_mouth_on) {
        int R = imax(5, (int)(8 * fminf(FF_W / 128.0f, FF_H / 64.0f)));
        ffArcSmile(mx, imin(my, FF_H - 2 - R) + dy, R);
    }
}

static void ffSmugEye(int cx, int cy, int ew, int h, int lid, int slant, bool inner_right)
{
    int x = cx - ew / 2, y = cy - h / 2;
    ffOn();  ffRBox(x, y, ew, h, (int)(imin(ew, h) * 0.4f));
    ffOff(); ffBox(x - 1, y - 1, ew + 2, lid + 1);
    if (inner_right) ffTri(x - 1, y + lid, x + ew + 1, y + lid, x + ew + 1, y + lid + slant);
    else             ffTri(x - 1, y + lid, x + ew + 1, y + lid, x - 1, y + lid + slant);
}

static void ffSmug(int lx, int ly, int rx, int ry, int ew, int eh, int mx, int my, int mw, int mh, float bf)
{
    uint32_t c = g_now % 4200;
    int G = ew / 4, gx = 0;
    if (c >= 800 && c < 1200) gx = (int)(G * ffEase((float)(c - 800) / 400.0f));
    else if (c >= 1200 && c < 3400) gx = G;
    else if (c >= 3400 && c < 3800) gx = (int)(G * (1.0f - ffEase((float)(c - 3400) / 400.0f)));
    int h = imax(4, (int)(eh * bf));
    int lid_base = (int)(h * 0.48f);
    bool wiggle = (c >= 1600 && c < 1800) || (c >= 2000 && c < 2200);
    int slant = imax(2, (int)(eh * 0.16f));
    ffSmugEye(lx + gx, ly, ew, h, wiggle ? (int)(h * 0.22f) : lid_base, slant, true);
    ffSmugEye(rx + gx, ry, ew, h, lid_base, slant, false);
    if (g_mouth_on) ffSmirk(mx + gx / 2, my, mw, mh);
}

static void ffStar(int cx, int cy, int Rh, int Rv, int k)
{
    ffTri(cx, cy - Rv, cx - k, cy, cx + k, cy);
    ffTri(cx, cy + Rv, cx - k, cy, cx + k, cy);
    ffTri(cx - Rh, cy, cx, cy - k, cx, cy + k);
    ffTri(cx + Rh, cy, cx, cy - k, cx, cy + k);
}

static void ffStarEyes(int lx, int ly, int rx, int ry, int ew, int eh, int mx, int my, int mw, float bf)
{
    float pulse = 0.85f + 0.15f * sinf(g_now * 0.008f);
    int Rh = imax(5, (int)(ew * 0.62f * pulse));
    int Rv = imax(5, (int)(eh * 0.45f * pulse * bf));
    int k = imax(2, Rh / 3);
    ffOn();
    ffStar(lx, ly, Rh, Rv, k);
    ffStar(rx, ry, Rh, Rv, k);
    int tw[3][2] = {{lx - ew, ly - (int)(eh * 0.35f)}, {rx + ew, ry - (int)(eh * 0.15f)}, {(lx + rx) / 2, ly - (int)(eh * 0.45f)}};
    uint32_t phase = g_now / 300;
    for (int i = 0; i < 3; i++) {
        if ((phase + (uint32_t)i) % 3 != 0) {
            ffLine(tw[i][0] - 2, tw[i][1], tw[i][0] + 2, tw[i][1]);
            ffLine(tw[i][0], tw[i][1] - 2, tw[i][0], tw[i][1] + 2);
        }
    }
    if (g_mouth_on) {
        float gasp = 0.85f + 0.15f * sinf(g_now * 0.006f);
        int ow = imax(6, (int)(mw * 0.6f * gasp));
        int oh = imax(6, (int)(mw * 0.8f * gasp));
        int oy = imin(my - oh / 2, FF_H - 1 - oh);
        ffRBox(mx - ow / 2, oy, ow, oh, imin(ow, oh) / 2);
    }
}

static void ffCool(int lx, int ly, int rx, int ew, int eh, int mx, int my, int mw, int mh)
{
    int dy = (int)(sinf(g_now * 0.004f) * 1.5f);
    int gw = (int)(ew * 1.35f), gh = imax(6, (int)(eh * 0.42f));
    int gy = ly - gh / 2 - 2 + dy;
    int xl = lx - gw / 2, xr = rx - gw / 2;
    ffOn();
    ffRBox(xl, gy, gw, gh, 3);
    ffRBox(xr, gy, gw, gh, 3);
    ffThick(xl + gw - 1, gy + 1, xr, gy + 1);
    ffThick(xl, gy + 1, xl - 5, gy - 1);
    ffThick(xr + gw - 1, gy + 1, xr + gw + 4, gy - 1);
    uint32_t p = g_now % 2600;
    if (p < 500) {
        int x = xl + (int)(((float)p / 500.0f) * (float)(gw + gh));
        ffOff();
        ffThick(x, gy + 1, x - gh + 2, gy + gh - 2);
        ffThick(x + (xr - xl), gy + 1, x + (xr - xl) - gh + 2, gy + gh - 2);
    }
    if (g_mouth_on) ffSmirk(mx, my + dy, mw, mh);
}

static void ffCheeky(int lx, int ly, int rx, int ry, int ew, int eh, int mx, int my, int mw, int mh, float bf)
{
    int tilt = (int)(sinf(g_now * 0.003f) * 3);
    int h = imax(3, (int)(eh * bf));
    ffOn();
    ffRBox(lx - ew / 2 + tilt, ly - h / 2, ew, h, (int)(imin(ew, h) * 0.4f));
    int sw = (int)(ew * 1.1f);
    ffRBox(rx - sw / 2 + tilt, ry + (int)(eh * 0.08f), sw, 4, 1);
    if (g_mouth_on) {
        int bw = (int)(mw * 0.7f), bh = imax(3, mh);
        ffRBox(mx - bw + tilt, my - bh + 1, 2 * bw, bh, 1);
        int tw = imax(6, (int)(mw * 0.8f));
        int th = imax(4, imin(10, FF_H - my - 1));
        int tx = mx - tw / 2 + tilt + (int)(sinf(g_now * 0.02f) * 3);
        ffRBox(tx, my - 1, tw, th, 3);
        ffOff();
        ffLine(tx + tw / 2, my + 1, tx + tw / 2, my + th - 4);
    }
}

static void ffDerp(int lx, int ly, int rx, int ry, int ew, int eh, int mx, int my, int mw, int mh, float bf)
{
    int bw = (int)(ew * 1.1f), bh = imax(3, (int)(eh * 0.95f * bf));
    int sw = (int)(ew * 0.6f), sh = imax(3, (int)(eh * 0.5f * bf));
    float a1 = g_now * 0.006f, a2 = -(g_now * 0.009f) + 1;
    int l0 = lx + (int)(cosf(a1) * 3), l1 = ly + (int)(sinf(a1) * 3);
    int r0 = rx + (int)(cosf(a2) * 3), r1 = ry + 4 + (int)(sinf(a2) * 3);
    ffOn();
    ffRBox(l0 - bw / 2, l1 - bh / 2, bw, bh, (int)(imin(bw, bh) * 0.45f));
    ffRBox(r0 - sw / 2, r1 - sh / 2, sw, sh, (int)(imin(sw, sh) * 0.45f));
    if (g_mouth_on) {
        int rr = imax(1, mh / 2), px = 0, py = 0;
        for (int s = 0; s <= 6; s++) {
            int x = mx - mw + (2 * mw * s) / 6;
            int y = my - 2 + (int)(sinf(s * 1.3f + g_now * 0.01f) * 1.5f);
            if (s > 0) ffCapsule(px, py, x, y, rr);
            px = x; py = y;
        }
        ffBox(mx - 4, my, 3, 5);
        ffBox(mx + 1, my, 3, 5);
    }
}

/* ============================================================
 * DOC — 7. DESENHO DE CADA EXPRESSÃO
 * ============================================================ */

static void mouth_rrect(int x, int y, int w, int h, int r)
{
    if (g_mouth_on) v_rrect(x, y, w, h, r, ON);
}

static void drawFace(doc_emotion_t emo)
{
    switch (emo) {
    case DOC_EMO_CUSTOM:
    case DOC_EMO_ALIVE:
    default: {
        int l_base = is_yawning ? 34 : 75;
        int l_h = imax(2, (int)(l_base * blink_factor));
        int l_y = 92 + (75 - l_h) / 2 + (is_yawning ? 15 : 0);
        drawEye(53 + eye_ox, l_y + eye_oy, 45, l_h, 20);
        int r_base = is_yawning ? 34 : 75;
        int r_h = imax(2, (int)(r_base * blink_factor));
        int r_y = 92 + (75 - r_h) / 2 + (is_yawning ? 15 : 0);
        drawEye(143 + eye_ox, r_y + eye_oy, 45, r_h, 20);
        mouth_rrect(105 + eye_ox / 2, 179 - mouth_extra_h / 2, 30, 8 + mouth_extra_h, 3);
        break;
    }

    case DOC_EMO_HAPPY: {
        float t = g_now * 0.005f;
        float pulse_h = 0.95f + sinf(t) * 0.18f;
        float pulse_w = 0.96f + cosf(t) * 0.08f;
        int eh = imax(4, (int)(71 * pulse_h * blink_factor));
        int ew = imax(4, (int)(40 * pulse_w));
        int cy = 84 + (71 - eh) / 2;
        drawEye(55 + (40 - ew) / 2 + eye_ox, cy + eye_oy, ew, eh, 15);
        drawEye(145 + (40 - ew) / 2 + eye_ox, cy + eye_oy, ew, eh, 15);
        if (g_mouth_on) {
            float wt = g_now * 0.0055f;
            int bob = (int)(sinf(wt) * 4.5f);
            int ww = imax(6, (int)(54 * 0.40f + sinf(wt * 2.0f) * 1.5f));
            int wh = imax(6, (int)(18 * 1.25f + cosf(wt * 2.0f) * 1.0f));
            int wx = 93 + eye_ox / 2 + (54 - ww) / 2;
            int wy = 174 - mouth_extra_h / 2 - bob;
            v_rrect(wx, wy, ww, wh, imin(ww, wh) / 2, ON);
            int phase = (int)((g_now / 28) % 45);
            int nx = wx + ww + 4 + phase / 3;
            int ny = wy - 4 - phase / 2;
            if (ny > 5 && nx < 234) {
                v_circle(nx, ny, 1, ON);
                v_line(nx + 1, ny, nx + 1, ny - 5, ON);
                v_line(nx + 1, ny - 5, nx + 3, ny - 4, ON);
            }
        }
        break;
    }

    case DOC_EMO_SAD: {
        int h = imax(4, (int)(75 * blink_factor));
        int y = 92 + (75 - h) / 2;
        drawEye(60 + eye_ox, y + eye_oy, 45, h, 20);
        drawEye(136 + eye_ox, y + eye_oy, 45, h, 20);
        drawTears(83 + eye_ox, 159 + eye_ox, 167 + eye_oy);
        mouth_rrect(109 + eye_ox / 2, 185 + 2, 23, 7, 2);
        break;
    }

    case DOC_EMO_ANGRY: {
        drawAngryVein(176, 116);
        int l_base = is_yawning ? 27 : 60;
        int l_h = imax(2, (int)(l_base * blink_factor));
        int l_y = 107 + (60 - l_h) / 2 + (is_yawning ? 12 : 0);
        drawEye(69 + eye_ox, l_y + eye_oy, 41, l_h, 18);
        int r_base = is_yawning ? 21 : 47;
        int r_h = imax(2, (int)(r_base * blink_factor));
        int r_y = 120 + (47 - r_h) / 2 + (is_yawning ? 9 : 0);
        drawEye(131 + eye_ox, r_y + eye_oy, 41, r_h, 18);
        mouth_rrect(99 + eye_ox / 2, 179 - mouth_extra_h / 2, 42, 8 + mouth_extra_h, 3);
        break;
    }

    case DOC_EMO_LOVE: {
        float beat = 0.92f + 0.16f * fabsf(sinf(g_now * 0.005f));
        int w = imax(4, (int)(45 * beat));
        int h = imax(2, (int)(75 * beat * blink_factor));
        drawEye(53 + (45 - w) / 2 + eye_ox, 92 + (75 - h) / 2 + eye_oy, w, h, 20);
        drawEye(143 + (45 - w) / 2 + eye_ox, 92 + (75 - h) / 2 + eye_oy, w, h, 20);
        drawFloatingHearts();
        if (g_mouth_on) drawSmileArc(120 + eye_ox / 2, 183, 15);
        break;
    }

    case DOC_EMO_DIZZY:
        drawSpiral(76 + eye_ox, 130 + eye_oy, -1, spiral_angle);
        drawSpiral(166 + eye_ox, 130 + eye_oy, 1, spiral_angle);
        spiral_angle += 0.25f;
        if (g_mouth_on) v_circle_outline(108 + eye_ox / 2, 179, 5, ON);
        break;

    case DOC_EMO_WINK: {
        bool wink_right = (g_now % 8000u) < 5000u;
        int h = imax(2, (int)(75 * blink_factor));
        int y = 92 + (75 - h) / 2;
        drawEye((wink_right ? 53 : 143) + eye_ox, y + eye_oy, 45, h, 20);
        drawEye((wink_right ? 143 : 53) - 3 + eye_ox, 135 + eye_oy, 50, 4, 1);
        mouth_rrect(101 + eye_ox / 2, 177, 39, 12, 4);
        break;
    }

    case DOC_EMO_CONFUSED: {
        uint32_t ct = g_now % 7600;
        int mid_x = 120, mid_y = 140;
        float scale = 1.0f;
        int ox = 0, oy = 0;
        float mw_s = 1.0f, mh_s = 1.0f;
        float cur_blink = blink_factor;

        if (ct < 2000) {
            scale = 1.0f; mw_s = 0.65f; mh_s = 0.75f;
            if (ct < 500) { ox = -18; oy = -8; }
            else if (ct < 1000) { ox = 18; oy = -8; }
            else if (ct < 1500) { ox = 16; oy = 8; }
            else { ox = -16; oy = 8; }
        } else if (ct < 4600) {
            scale = 1.68f; mw_s = 1.35f; mh_s = 1.95f;
            uint32_t t = ct - 2000;
            if (t < 650) { ox = -24; oy = -13; }
            else if (t < 1300) { ox = 24; oy = -13; }
            else if (t < 1950) { ox = 22; oy = 12; }
            else { ox = -22; oy = 12; }
        } else if (ct < 6400) {
            scale = 0.95f;
            uint32_t t = ct - 4600;
            int edge = (int)(t / 450), sub = (int)(t % 450);
            if (sub < 200) { mw_s = 0.45f; mh_s = 0.55f; } else { mw_s = 1.30f; mh_s = 1.70f; }
            if (edge == 0) { ox = -24; oy = 0; }
            else if (edge == 1) { ox = 0; oy = -13; }
            else if (edge == 2) { ox = 24; oy = 0; }
            else { ox = 0; oy = 12; }
        } else {
            scale = 1.0f; mw_s = 0.75f; mh_s = 1.15f;
            if ((ct >= 6700 && ct <= 6850) || (ct >= 7150 && ct <= 7300)) cur_blink = 0.08f;
        }

        /* DOC — na faixa superior o zoom de 1,68x estouraria os 32 px: limita. */
        if (g_app_p > 0.0f && scale > 1.0f) {
            scale = 1.0f;
            ox /= 2;
            oy /= 2;
        }

        int lw = imax(4, (int)(43 * scale)), lh = imax(2, (int)(71 * scale * cur_blink));
        int lx = mid_x + (int)((54 - mid_x) * scale) + ox;
        int ly = mid_y + (int)((94 - mid_y) * scale) + oy;
        lx = clampi(lx, 0, 240 - lw); ly = clampi(ly, 0, 280 - lh);
        drawEye(lx, ly, lw, lh, imin(lw, lh) / 2);

        int rw = imax(4, (int)(45 * scale)), rh = imax(2, (int)(75 * scale * cur_blink));
        int rx = mid_x + (int)((143 - mid_x) * scale) + ox;
        int ry = mid_y + (int)((92 - mid_y) * scale) + oy;
        rx = clampi(rx, 0, 240 - rw); ry = clampi(ry, 0, 280 - rh);
        drawEye(rx, ry, rw, rh, imin(rw, rh) / 2);

        int mw = imax(4, (int)(24 * scale * mw_s)), mh = imax(3, (int)(11 * scale * mh_s));
        int mx = mid_x + (int)((108 - mid_x) * scale) + ox;
        int my = mid_y + (int)((178 - mid_y) * scale) + oy;
        mx = clampi(mx, 0, 240 - mw); my = clampi(my, 0, 280 - mh);
        mouth_rrect(mx, my, mw, mh, imin(mw, mh) / 2);
        break;
    }

    case DOC_EMO_SLEEPY: {
        int l_base = is_yawning ? 7 : 15;
        int l_h = imax(2, (int)(l_base * blink_factor));
        int l_y = 144 + (15 - l_h) / 2 + (is_yawning ? 3 : 0);
        drawEye(53 + eye_ox, l_y + eye_oy, 45, l_h, 6);
        int r_base = is_yawning ? 7 : 15;
        int r_h = imax(2, (int)(r_base * blink_factor));
        int r_y = 144 + (15 - r_h) / 2 + (is_yawning ? 3 : 0);
        drawEye(143 + eye_ox, r_y + eye_oy, 45, r_h, 6);
        mouth_rrect(113 + eye_ox / 2, 179 - mouth_extra_h / 2, 15, 8 + mouth_extra_h, 3);
        break;
    }

    case DOC_EMO_DEEP_SLEEP: {
        drawEye(53 + eye_ox, 151 + eye_oy, 45, 3, 1);
        drawEye(143 + eye_ox, 151 + eye_oy, 45, 3, 1);
        float st = g_now * 0.0028f;
        float prog = (sinf(st) + 1.0f) * 0.5f;
        if (g_mouth_on) {
            int mh = imax(3, (int)(12 * (0.45f + prog * 1.1f)));
            int mw = imax(6, (int)(20 * (0.75f + prog * 0.4f)));
            int my = 177 - mh / 2;
            int mx = 110 + eye_ox / 2 + (20 - mw) / 2;
            v_rrect(mx, my, mw, mh, imin(mw, mh) / 2, ON);
            int dx = mx + mw - 1, dy = my + mh / 2;
            int len = 4 + (int)(prog * 2.5f);
            v_line(dx, dy, dx, dy + len, ON);
            v_circle(dx, dy + len, 1, ON);
        }
        for (int i = 0; i < 3; i++) {
            uint32_t zp = (g_now + (uint32_t)i * 800u) % 2400u;
            float zprog = (float)zp / 2400.0f;
            int zs = 4 + i * 2;
            int zx = 166 + eye_ox + (int)(zprog * 18.0f);
            int zy = 148 + eye_oy - (int)(zprog * 22.0f);
            if (zy > 4 && zx < 234 && zprog < 0.9f) drawZ(zx, zy, zs);
        }
        break;
    }

    case DOC_EMO_FURIOUS: {
        drawAngryVein(174, 110);
        bool twitch = (g_now / 240) % 4 == 0;
        int l_h = twitch ? imax(2, (int)(75 * 0.62f * blink_factor)) : imax(2, (int)(75 * blink_factor));
        int l_y = 92 + (75 - l_h) / 2 + (twitch ? 1 : 0);
        drawEye(71 + eye_ox, l_y + eye_oy, 43, l_h, 19);
        int r_h = imax(2, (int)(53 * blink_factor));
        int r_y = 114 + (53 - r_h) / 2;
        drawEye(127 + eye_ox, r_y + eye_oy, 43, r_h, 19);
        int mutter = (int)(fabsf(sinf(g_now * 0.028f)) * 6);
        int mh = imax(2, 13 + mutter + mouth_extra_h);
        int mw = imax(4, 45 + mutter / 2);
        mouth_rrect(98 + eye_ox / 2 - mutter / 4, 177 - mh / 2, mw, mh, 5);
        break;
    }

    case DOC_EMO_SMILE:
        ffSmile(76 + eye_ox, 130 + eye_oy, 166 + eye_ox, 130 + eye_oy, 45, 75, 120 + eye_ox, 183, blink_factor);
        ffOn();
        break;

    case DOC_EMO_SMUG:
        ffSmug(75 + eye_ox, 130 + eye_oy, 165 + eye_ox, 130 + eye_oy, 52, 75, 120 + eye_ox, 183, 38, 8, blink_factor);
        ffOn();
        break;

    case DOC_EMO_STAR_EYES:
        ffStarEyes(76 + eye_ox, 130 + eye_oy, 166 + eye_ox, 130 + eye_oy, 45, 75, 120 + eye_ox, 183, 30, blink_factor);
        ffOn();
        break;

    case DOC_EMO_COOL:
        ffCool(76 + eye_ox, 130 + eye_oy, 166 + eye_ox, 45, 75, 120 + eye_ox, 183, 36, 8);
        ffOn();
        break;

    case DOC_EMO_CHEEKY:
        ffCheeky(76 + eye_ox, 130 + eye_oy, 166 + eye_ox, 130 + eye_oy, 45, 75, 120 + eye_ox, 183, 30, 8, blink_factor);
        ffOn();
        break;

    case DOC_EMO_DERP:
        ffDerp(76 + eye_ox, 130 + eye_oy, 166 + eye_ox, 130 + eye_oy, 45, 75, 120 + eye_ox, 183, 30, 8, blink_factor);
        ffOn();
        break;
    }
}

/* ============================================================
 * DOC — 8. ANIMAÇÕES: piscar, olhar em volta, bocejar e falar
 * ============================================================ */

static void start_blink(uint32_t now, uint32_t duration)
{
    is_blinking = true;
    blink_start = now;
    blink_dur = duration;
}

static void update_idle_animations(uint32_t now)
{
    if (!is_blinking && (int32_t)(now - next_blink_time) >= 0) {
        start_blink(now, (uint32_t)rnd(120, 160));
        next_blink_time = now + (uint32_t)rnd(2000, 5500);
        if (rnd(0, 100) < 20) next_blink_time = now + (uint32_t)rnd(240, 340);
    }

    if (is_blinking) {
        uint32_t elapsed = now - blink_start;
        if (elapsed < blink_dur) {
            float progress = (float)elapsed / (float)blink_dur;
            blink_factor = fmaxf(0.06f, 1.0f - sinf(progress * 3.14159f) * 0.94f);
        } else {
            is_blinking = false;
            blink_factor = 1.0f;
        }
    } else {
        blink_factor = 1.0f;
    }

    if (g_bias_on) {
        tgt_eye_ox = g_bias_x;
        tgt_eye_oy = g_bias_y;
        look_reset_time = 0;
    } else {
        if ((int32_t)(now - next_look_time) >= 0) {
            tgt_eye_ox = (float)rnd(-4, 5);
            tgt_eye_oy = (float)rnd(-3, 4);
            next_look_time = now + (uint32_t)rnd(2500, 5000);
            look_reset_time = now + (uint32_t)rnd(1400, 2200);
        }
        if (look_reset_time > 0 && (int32_t)(now - look_reset_time) >= 0) {
            tgt_eye_ox = 0.0f;
            tgt_eye_oy = 0.0f;
            look_reset_time = 0;
        }
    }
    cur_eye_ox += (tgt_eye_ox - cur_eye_ox) * 0.22f;
    cur_eye_oy += (tgt_eye_oy - cur_eye_oy) * 0.22f;
    eye_ox = iround(cur_eye_ox);
    eye_oy = iround(cur_eye_oy);

    if (!is_yawning && (int32_t)(now - next_yawn_time) >= 0) {
        is_yawning = true;
        yawn_end_time = now + 850;
        next_yawn_time = now + (uint32_t)rnd(30000, 65000);
    }
    if (is_yawning && (int32_t)(now - yawn_end_time) >= 0) {
        is_yawning = false;
    }

    mouth_extra_h = is_yawning ? 11 : 0;
    if (g_talking) {
        /* DOC — boca abrindo e fechando enquanto fala. */
        mouth_extra_h = 3 + (int)(9.0f * fabsf(sinf(now * 0.021f)));
    }
}

/* ============================================================
 * DOC — 9. API PÚBLICA
 * ============================================================ */

void doc_face_init(uint16_t face_rgb565, uint16_t bg_rgb565)
{
    g_fg = face_rgb565;
    g_bg = bg_rgb565;
    g_emo = DOC_EMO_ALIVE;
    cur_eye_ox = cur_eye_oy = tgt_eye_ox = tgt_eye_oy = 0.0f;
    eye_ox = eye_oy = 0;
    is_blinking = false;
    blink_factor = 1.0f;
    is_yawning = false;
    mouth_extra_h = 0;
    g_talking = false;
    g_bias_on = false;
    g_app_mode = false;
    g_app_p = 0.0f;
    g_band_settled = false;
    g_started = false;
    spiral_angle = 0.0f;
    tear_progress = 0.0f;
}

void doc_face_set_emotion(doc_emotion_t emotion)
{
    if ((int)emotion < 0 || emotion >= DOC_EMO_COUNT) emotion = DOC_EMO_ALIVE;
    if (emotion != g_emo) {
        g_emo = emotion;
        start_blink(g_now, 120);   /* piscada de transição, como no sketch */
    }
}

doc_emotion_t doc_face_get_emotion(void) { return g_emo; }

void doc_face_set_gaze_bias(bool enabled, float x, float y)
{
    g_bias_on = enabled;
    g_bias_x = x;
    g_bias_y = y;
}

void doc_face_set_talking(bool talking) { g_talking = talking; }
void doc_face_set_app_mode(bool enabled) { g_app_mode = enabled; }
bool doc_face_get_app_mode(void) { return g_app_mode; }

void doc_face_blink_now(void)
{
    if (!is_blinking) start_blink(g_now, 140);
}

float doc_face_app_progress(void) { return g_app_p; }

void doc_face_tick(uint32_t now_ms)
{
    g_now = now_ms;

    if (!g_started) {
        g_started = true;
        g_last_tick = now_ms;
        next_blink_time = now_ms + 2000;
        next_look_time = now_ms + 2500;
        look_reset_time = 0;
        next_yawn_time = now_ms + 25000;
    }

    float dt = (float)(now_ms - g_last_tick) / 1000.0f;
    g_last_tick = now_ms;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f;

    float step = dt / APP_ANIM_SECONDS;
    if (g_app_mode) g_app_p = fminf(1.0f, g_app_p + step);
    else            g_app_p = fmaxf(0.0f, g_app_p - step);

    update_idle_animations(now_ms);
}

int doc_face_render(uint16_t *fb, int stride_px)
{
    g_fb = fb;
    g_stride = stride_px;

    float e = smooth01(g_app_p);
    g_s = lerpf(S_FULL, S_BAND, e);
    g_ox = lerpf(OX_FULL, OX_BAND, e);
    g_oy = lerpf(OY_FULL, OY_BAND, e);

    /* Com app aberto (ou abrindo) a boca vira o contorno: não desenha a boca. */
    g_mouth_on = (g_app_p <= 0.0f);

    int rows = (g_app_p >= 1.0f && g_band_settled) ? DOC_FACE_BAND_H : DOC_FACE_H;
    g_rows = rows;

    for (int y = 0; y < rows; y++) {
        uint16_t *row = fb + y * stride_px;
        for (int x = 0; x < DOC_FACE_W; x++) row[x] = g_bg;
    }

    g_col = ON;
    drawFace(g_emo);

    g_band_settled = (g_app_p >= 1.0f);
    return rows;
}

bool doc_face_get_frame(doc_frame_t *frame)
{
    if (frame == NULL || g_app_p <= 0.0f) return false;

    float e = smooth01(g_app_p);

    float x0 = OX_FULL + MOUTH_X * S_FULL, y0 = OY_FULL + MOUTH_Y * S_FULL;
    float w0 = MOUTH_W * S_FULL, h0 = MOUTH_H * S_FULL;

    frame->x = iround(lerpf(x0, 0.0f, e));
    frame->y = iround(lerpf(y0, (float)DOC_FACE_APP_Y, e));
    frame->w = imax(1, iround(lerpf(w0, (float)DOC_FACE_W, e)));
    frame->h = imax(1, iround(lerpf(h0, (float)DOC_FACE_APP_H, e)));
    frame->radius = iround(lerpf(2.0f, (float)FRAME_RADIUS, e));
    frame->border = iround(lerpf(0.0f, (float)FRAME_BORDER, e));
    frame->fill_opa = (uint8_t)iround(255.0f * (1.0f - e));
    return true;
}
