// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// PS4 status screen: while Eden runs without a renderer (bring-up, benchmarks) the TV would only
// show black. This draws its own 1080p frame through VideoOut a few times a second: what Eden is
// doing (the boot-trace lines), for how long, and the contents of a file the guest writes (e.g.
// the benchmark's results), so progress is visible on the console itself.
//
// Independent of Eden's video code: CPU-drawn into an off-screen buffer, copied into one of two
// linear direct-memory framebuffers and flipped. Must be stopped before a real renderer opens
// VideoOut (one handle per output bus).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <cmath>
#include <vector>

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ps4_launcher.h"
#include "status_font.h"
#include "status_font_bold.h"
#include "status_font_title.h"
#include "status_logo.h"

// New pipelines built in game (stub.cpp; the loading screen shows them).
extern std::atomic<unsigned long long> ps4_pipelines_built;

extern "C" {
int32_t sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void* param);
int32_t sceVideoOutClose(int32_t handle);
int32_t sceVideoOutRegisterBuffers(int32_t handle, int32_t start, void* const* addresses, int32_t count,
                                   const void* attribute);
int32_t sceVideoOutUnregisterBuffers(int32_t handle, int32_t set);
int32_t sceVideoOutSubmitFlip(int32_t handle, int32_t index, uint32_t mode, int64_t arg);
int32_t sceVideoOutSetFlipRate(int32_t handle, int32_t rate);
int32_t sceVideoOutIsFlipPending(int32_t handle);
size_t sceKernelGetDirectMemorySize();
int32_t sceKernelAllocateDirectMemory(off_t start, off_t end, size_t length, size_t alignment, int32_t type,
                                      off_t* phys);
int32_t sceKernelMapDirectMemory(void** addr, size_t length, int32_t prot, int32_t flags, off_t phys,
                                 size_t alignment);
int32_t sceKernelMunmap(void* addr, size_t length);
int32_t sceKernelReleaseDirectMemory(off_t start, size_t length);
}

namespace {

constexpr int kWidth = 1920, kHeight = 1080, kBuffers = 2;
constexpr size_t kFrameBytes = size_t(kWidth) * kHeight * 4;
constexpr size_t kFrameAligned = (kFrameBytes + (2u << 20) - 1) & ~size_t((2u << 20) - 1);
constexpr int32_t kUserIdSystem = 0xFF, kBusMain = 0, kFlipVsync = 1;
constexpr int32_t kWcGarlic = 3, kProtCpuGpuRw = 0x3 | 0x30;

struct BufferAttribute { // OrbisVideoOutBufferAttribute
    int32_t format, tmode, aspect;
    uint32_t width, height, pixel_pitch;
    uint64_t reserved[2];
};

// Colours in the framebuffer's A8B8G8R8 layout (little endian: 0xAABBGGRR).
constexpr uint32_t Rgb(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | r;
}
constexpr uint32_t kBackground = Rgb(14, 20, 34), kPanel = Rgb(24, 33, 54), kAccent = Rgb(90, 170, 255);
constexpr uint32_t kText = Rgb(228, 233, 242), kDim = Rgb(140, 152, 175), kGood = Rgb(110, 220, 140),
                   kBad = Rgb(255, 110, 110);

// Recent trace lines: fixed storage, no allocation (ps4_boot_trace is called from inside malloc).
constexpr int kLines = 64, kLineLen = 160;
char g_lines[kLines][kLineLen];
int g_line_count = 0;
char g_stage[kLineLen] = "Starting";
char g_title[64] = "PS4 Switch Emu";
char g_watch_path[256] = "";
char g_watch_heading[64] = "";
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
std::atomic<bool> g_running{false}, g_stop{false}, g_crashed{false}, g_ready{false};
pthread_t g_thread;
timespec g_start;

// Watched-file lines as shown: the text plus, for "end <test>..." lines, the real time since the
// matching "begin <test>" (the guest can't time itself: its clock follows emulated cycles in
// single-core mode). Each new line is also written to boot-trace.log with the real time.
constexpr int kWatchLines = 24;
char g_watch_lines[kWatchLines][kLineLen];
int g_watch_count = 0;
char g_begin_name[kLineLen] = "";
double g_begin_time = 0.0;

// Menu (ps4_menu.cpp): when set, drawn in place of the log. A line starting with "> " is the
// selected one.
constexpr int kMenuLines = 20;
char g_menu[kMenuLines][kLineLen];
int g_menu_count = 0;

void Lock() {
    while (g_lock.test_and_set(std::memory_order_acquire)) {
    }
}
void Unlock() {
    g_lock.clear(std::memory_order_release);
}

void CopyText(char* dst, size_t size, const char* src) {
    size_t i = 0;
    for (; src && src[i] && src[i] != '\n' && i + 1 < size; i++)
        dst[i] = (src[i] >= 32 && src[i] < 127) ? src[i] : ' ';
    dst[i] = 0;
}

double Elapsed();
extern "C" __attribute__((weak)) void ps4_boot_trace(const char* stage);

// Reads the watched file and takes in lines it hasn't seen yet (status thread only).
void PollWatchedFile(const char* path) {
    FILE* f = path[0] ? fopen(path, "r") : nullptr;
    if (!f)
        return;
    char line[kLineLen];
    int index = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strchr(line, '\n'))
            break; // still being written
        if (index++ < g_watch_count)
            continue;
        char clean[kLineLen];
        CopyText(clean, sizeof(clean), line);
        const double now = Elapsed();
        char shown[kLineLen];
        snprintf(shown, sizeof(shown), "%s", clean);
        if (strncmp(clean, "begin ", 6) == 0) {
            CopyText(g_begin_name, sizeof(g_begin_name), clean + 6);
            g_begin_time = now;
        } else if (strncmp(clean, "end ", 4) == 0 && g_begin_name[0] &&
                   strncmp(clean + 4, g_begin_name, strlen(g_begin_name)) == 0) {
            snprintf(shown, sizeof(shown), "%.90s  -> %.1f s real", clean, now - g_begin_time);
            g_begin_name[0] = 0;
        }
        Lock();
        if (g_watch_count < kWatchLines)
            CopyText(g_watch_lines[g_watch_count], kLineLen, shown);
        g_watch_count++;
        Unlock();
        if (ps4_boot_trace) {
            char traced[kLineLen + 16];
            snprintf(traced, sizeof(traced), "output: %s", shown);
            ps4_boot_trace(traced);
        }
    }
    fclose(f);
}

// --- drawing into the off-screen frame ---------------------------------------------------------

uint32_t* g_frame; // kWidth * kHeight

void FillRect(int x, int y, int w, int h, uint32_t color) {
    for (int row = y; row < y + h && row < kHeight; row++)
        for (int col = x; col < x + w && col < kWidth; col++)
            g_frame[row * kWidth + col] = color;
}

// d toward s by a/256, all channels at once (packed: red+blue, then green).
inline uint32_t Lerp(uint32_t d, uint32_t s, uint32_t a) {
    const uint32_t rb = (((s & 0xFF00FFu) * a + (d & 0xFF00FFu) * (256 - a)) >> 8) & 0xFF00FFu;
    const uint32_t g = (((s & 0x00FF00u) * a + (d & 0x00FF00u) * (256 - a)) >> 8) & 0x00FF00u;
    return 0xFF000000u | rb | g;
}

// src over dst with alpha 0..255.
inline uint32_t Blend(uint32_t dst, uint32_t src, uint32_t alpha) {
    return Lerp(dst, src, alpha + (alpha >> 7)); // 0..255 -> 0..256
}

struct Font {
    int height;
    const StatusGlyph* glyphs;
    const uint8_t* alpha;
};
constexpr Font kRegular{kStatusFontHeight, kStatusGlyphs, kStatusFontAlpha};
constexpr Font kBold{kBoldFontHeight, kBoldGlyphs, kBoldFontAlpha};
constexpr Font kTitleFont{kTitleFontHeight, kTitleGlyphs, kTitleFontAlpha};

// Draws one line of text; returns the x after it. Stops at max_x.
int DrawText(const Font& font, int x, int y, const char* text, uint32_t color, int max_x = kWidth - 40) {
    for (; *text; text++) {
        const int c = static_cast<unsigned char>(*text);
        if (c < 32 || c > 126)
            continue;
        const StatusGlyph& g = font.glyphs[c - 32];
        if (x + g.advance > max_x)
            break;
        const uint8_t* alpha = font.alpha + g.offset;
        for (int row = 0; row < font.height; row++) {
            const int py = y + row;
            if (py < 0 || py >= kHeight)
                continue;
            for (int col = 0; col < g.width; col++) {
                const int px = x + col;
                const uint32_t a = alpha[row * g.width + col];
                if (a && px >= 0 && px < kWidth)
                    g_frame[py * kWidth + px] = Blend(g_frame[py * kWidth + px], color, a);
            }
        }
        x += g.advance;
    }
    return x;
}

int DrawText(int x, int y, const char* text, uint32_t color, int max_x = kWidth - 40) {
    return DrawText(kRegular, x, y, text, color, max_x);
}

int TextWidth(const Font& font, const char* text) {
    int w = 0;
    for (; *text; text++) {
        const int c = static_cast<unsigned char>(*text);
        if (c >= 32 && c <= 126)
            w += font.glyphs[c - 32].advance;
    }
    return w;
}

double Elapsed() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return double(now.tv_sec - g_start.tv_sec) + double(now.tv_nsec - g_start.tv_nsec) / 1e9;
}

// --- launcher (start screen) ---------------------------------------------------------------------
//
// A games grid and a settings list over a background tinted with the selected game's colour.
// Rounded, anti-aliased cards with soft shadows; the selection ring, scrolling and the tint
// glide (the thread redraws at ~60 Hz while something moves, otherwise on changes only).

constexpr int kMaxGames = 128, kMaxSettings = 24;
constexpr int kCardSize = 160, kCardBig = 180; // icon sizes kept scaled per game (normal, selected)
constexpr int kRowTile = 300, kRowBig = 340;    // the row layout's cards (normal, selected)
constexpr int kLoadingCard = 360;               // the loading screen's card
struct GameEntry {
    char title[96];
    char detail[128];
    const uint32_t* icon;
    uint32_t accent; // the icon's average colour (or the tile colour)
    // The icon scaled (box filter) to the two sizes in use (a card and the selected card, in either
    // layout), made on first use.
    uint32_t* small = nullptr;
    uint32_t* big = nullptr;
    int small_size = 0, big_size = 0;
};
GameEntry g_games[kMaxGames];
int g_game_count = 0;
struct SettingEntry {
    char label[64];
    char value[96];
    char help[200];
    char group[32];
    int kind;
};
SettingEntry g_settings[kMaxSettings];
int g_setting_count = 0;
int g_tab = 0, g_game_sel = 0, g_setting_sel = 0, g_install_count = 0;
char g_scope[kLineLen] = "All games";
bool g_scope_switchable = false;
int g_layout = kLayoutGrid, g_shortcut = -1;
bool g_has_pad = true;
char g_toast[kLineLen] = "";
double g_toast_until = 0;
std::atomic<bool> g_launcher_on{false};
std::atomic<int> g_dirty{0};           // bumped on every change: the thread redraws at once
std::atomic<bool> g_animating{false};  // something is still moving: keep redrawing

constexpr uint32_t kCard = Rgb(30, 40, 66), kWhite = Rgb(255, 255, 255), kShadow = Rgb(0, 0, 0);
constexpr uint32_t kCrossBlue = Rgb(124, 178, 232), kCircleRed = Rgb(255, 102, 102),
                   kTriangleGreen = Rgb(64, 226, 160), kSquarePink = Rgb(255, 105, 248);

uint8_t R(uint32_t c) { return uint8_t(c); }
uint8_t G(uint32_t c) { return uint8_t(c >> 8); }
uint8_t B(uint32_t c) { return uint8_t(c >> 16); }
uint32_t Mix(uint32_t a, uint32_t b, float t) {
    return Rgb(uint8_t(R(a) + (R(b) - R(a)) * t), uint8_t(G(a) + (G(b) - G(a)) * t), uint8_t(B(a) + (B(b) - B(a)) * t));
}

// Animated state (status thread only).
struct Anim {
    float ring_x = -1, ring_y = 0, ring_size = 0; // selection ring
    float grid_scroll = 0, list_scroll = 0;       // pixels
    float tint_r = 30, tint_g = 60, tint_b = 120; // background tint
    float tab_x = -1, tab_w = 0;                  // tab underline
    float row_scroll = 0;                         // row layout: pixels
    float grow[kMaxGames] = {};                   // each card's growth toward the selected size, 0..1
    float shortcut_grow[kShortcutCount] = {};     // row layout: each shortcut's highlight, 0..1
    float bar_y = -1;                             // settings: the highlight bar, list space
    float toggle[kMaxSettings];                   // settings: each switch's knob, 0 (off) .. 1 (on)
    bool toggle_known[kMaxSettings] = {};
    float help_fade = 1;                          // settings: the help card's text fading in
    int help_row = -1;
    float page_fade = 0;                          // cross-fade from the previous page, 1 .. 0
    int last_tab = -1, last_layout = -1;
    char last_scope[96] = "";
    int sel_x = 0, sel_y = 0, sel_size = 0;       // where the selected card was last drawn
    double last = 0;
} g_anim;
uint32_t* g_prev = nullptr;   // the frame before a page change or a game's launch (cross-fades)
double g_launch_t0 = -1;      // a game is starting: the card zooms into the loading screen
bool g_launch_snapped = false;

// Approach for values in 0..1 (and Home menu item positions): snaps only when very close.
bool ApproachUnit(float& v, float target, float k) {
    if (std::fabs(target - v) < 0.004f) {
        v = target;
        return false;
    }
    v += (target - v) * k;
    return true;
}

// Smooth start and end for 0..1.
float Ease(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Blends the saved frame over rows y0.. of the current one (a = 256: all the saved frame).
void BlendPrev(int y0, uint32_t a) {
    if (!g_prev || a == 0)
        return;
    for (size_t i = size_t(y0) * kWidth; i < size_t(kWidth) * kHeight; i++)
        g_frame[i] = Lerp(g_frame[i], g_prev[i], a);
}

bool SnapshotFrame() {
    if (!g_prev)
        g_prev = static_cast<uint32_t*>(malloc(kFrameBytes));
    if (g_prev)
        memcpy(g_prev, g_frame, kFrameBytes);
    return g_prev != nullptr;
}

// Moves v toward target; true while it still moves.
bool Approach(float& v, float target, float k) {
    if (std::fabs(target - v) < 0.5f) {
        v = target;
        return false;
    }
    v += (target - v) * k;
    return true;
}

// Background: a vertical gradient tinted with the selected game's colour near the top.
void DrawBackground(uint32_t tint) {
    for (int y = 0; y < kHeight; y++) {
        const float t = float(y) / kHeight;
        const uint32_t base = Rgb(uint8_t(16 - 9 * t), uint8_t(20 - 12 * t), uint8_t(34 - 20 * t));
        const float glow = std::max(0.0f, 1.0f - t * 1.6f);
        const uint32_t row = Mix(base, tint, glow * glow * 0.28f);
        uint32_t* p = g_frame + size_t(y) * kWidth;
        for (int x = 0; x < kWidth; x++)
            p[x] = row;
    }
}

void DrawBackground() {
    DrawBackground(Rgb(uint8_t(g_anim.tint_r), uint8_t(g_anim.tint_g), uint8_t(g_anim.tint_b)));
}

// Repaints the background over rows [y0, y1): clips the scrolling grid/list under the header and
// above the footer.
void RepaintBackground(int y0, int y1) {
    const uint32_t tint = Rgb(uint8_t(g_anim.tint_r), uint8_t(g_anim.tint_g), uint8_t(g_anim.tint_b));
    for (int y = std::max(y0, 0); y < std::min(y1, kHeight); y++) {
        const float t = float(y) / kHeight;
        const uint32_t base = Rgb(uint8_t(16 - 9 * t), uint8_t(20 - 12 * t), uint8_t(34 - 20 * t));
        const float glow = std::max(0.0f, 1.0f - t * 1.6f);
        const uint32_t row = Mix(base, tint, glow * glow * 0.28f);
        uint32_t* p = g_frame + size_t(y) * kWidth;
        for (int x = 0; x < kWidth; x++)
            p[x] = row;
    }
}

void FillBlend(int x, int y, int w, int h, uint32_t color, uint32_t alpha) {
    for (int row = y < 0 ? 0 : y; row < y + h && row < kHeight; row++)
        for (int col = x < 0 ? 0 : x; col < x + w && col < kWidth; col++)
            g_frame[row * kWidth + col] = Blend(g_frame[row * kWidth + col], color, alpha);
}

// Coverage (0..255) of pixel (px, py) inside a rounded rectangle; corners anti-aliased.
inline uint32_t RoundCoverage(int px, int py, int x, int y, int w, int h, int r) {
    const int cx = px < x + r ? x + r : (px >= x + w - r ? x + w - r - 1 : px);
    const int cy = py < y + r ? y + r : (py >= y + h - r ? y + h - r - 1 : py);
    if (cx == px || cy == py)
        return 255;
    const float dx = float(px - cx), dy = float(py - cy);
    const float d = std::sqrt(dx * dx + dy * dy);
    const float c = float(r) + 0.5f - d;
    return c >= 1.0f ? 255 : c <= 0.0f ? 0 : uint32_t(c * 255.0f);
}

void RoundRect(int x, int y, int w, int h, int r, uint32_t color, uint32_t alpha = 255) {
    for (int py = std::max(y, 0); py < std::min(y + h, kHeight); py++) {
        uint32_t* row = g_frame + size_t(py) * kWidth;
        const bool corner_row = py < y + r || py >= y + h - r;
        for (int px = std::max(x, 0); px < std::min(x + w, kWidth); px++) {
            uint32_t a = alpha;
            if (corner_row && (px < x + r || px >= x + w - r))
                a = a * RoundCoverage(px, py, x, y, w, h, r) / 255;
            if (a == 255)
                row[px] = color;
            else if (a)
                row[px] = Blend(row[px], color, a);
        }
    }
}

// A soft drop shadow under a rounded rectangle. The shape's alpha is worked out once per size
// (four expanding rounded layers) and only pixels outside the rectangle itself are blended:
// the card covers the rest - cheap enough to redraw a full grid at 60 Hz on the PS4's CPU.
struct ShadowMask {
    int w = 0, h = 0, r = 0;
    std::vector<uint8_t> alpha; // (w + 2 * kPad) x (h + 2 * kPad + kDrop)
};
constexpr int kShadowPad = 12, kShadowDrop = 8;

const ShadowMask& GetShadowMask(int w, int h, int r) {
    static std::vector<ShadowMask> cache;
    for (const ShadowMask& m : cache)
        if (m.w == w && m.h == h && m.r == r)
            return m;
    ShadowMask m;
    m.w = w, m.h = h, m.r = r;
    const int mw = w + 2 * kShadowPad, mh = h + 2 * kShadowPad + kShadowDrop;
    m.alpha.assign(size_t(mw) * mh, 0);
    for (int i = 4; i >= 1; i--) {
        const int lx = kShadowPad - i * 3, ly = kShadowPad - i * 3 + kShadowDrop, lw = w + i * 6, lh = h + i * 6, lr = r + i * 3;
        for (int py = std::max(ly, 0); py < std::min(ly + lh, mh); py++)
            for (int px = std::max(lx, 0); px < std::min(lx + lw, mw); px++) {
                const uint32_t cover = 22 * RoundCoverage(px, py, lx, ly, lw, lh, lr) / 255;
                uint8_t& a = m.alpha[size_t(py) * mw + px];
                a = uint8_t(a + cover * (255 - a) / 255); // layers stack like blending black
            }
    }
    cache.push_back(std::move(m));
    return cache.back();
}

void Shadow(int x, int y, int w, int h, int r) {
    // A card between its resting sizes (growing, shrinking, the launch zoom): the shadow of the
    // nearest multiple of 20, centred, so a handful of masks serve every size in between.
    if (w == h && w != kCardSize && w != kCardBig && w != kRowTile && w != kRowBig && w != kLoadingCard) {
        const int q = std::max(20, (w + 10) / 20 * 20);
        x += (w - q) / 2, y += (h - q) / 2, w = h = q;
    }
    const ShadowMask& m = GetShadowMask(w, h, r);
    const int mw = w + 2 * kShadowPad, mh = h + 2 * kShadowPad + kShadowDrop;
    const int ox = x - kShadowPad, oy = y - kShadowPad;
    for (int my = 0; my < mh; my++) {
        const int py = oy + my;
        if (py < 0 || py >= kHeight)
            continue;
        const bool beside_only = py >= y + r && py < y + h - r; // rows where the card covers its width
        uint32_t* row = g_frame + size_t(py) * kWidth;
        const uint8_t* a = m.alpha.data() + size_t(my) * mw;
        for (int mx = 0; mx < mw; mx++) {
            const int px = ox + mx;
            if (beside_only && px >= x && px < x + w) {
                mx += w - 1; // skip what the card will cover
                continue;
            }
            if (a[mx] && px >= 0 && px < kWidth)
                row[px] = Blend(row[px], kShadow, a[mx]);
        }
    }
}

// Filled disc / ring and thick lines for the controller button symbols.
void Disc(int cx, int cy, int r, uint32_t color) {
    RoundRect(cx - r, cy - r, 2 * r + 1, 2 * r + 1, r, color);
}
void Ring(int cx, int cy, int r, int thick, uint32_t color) {
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++) {
            const float d = std::sqrt(float(x * x + y * y));
            const float c = std::min(float(r) + 0.5f - d, d - float(r - thick) + 0.5f);
            if (c > 0 && cy + y >= 0 && cy + y < kHeight && cx + x >= 0 && cx + x < kWidth) {
                uint32_t& p = g_frame[(cy + y) * kWidth + cx + x];
                p = Blend(p, color, c >= 1 ? 255 : uint32_t(c * 255));
            }
        }
}
void Line(int x0, int y0, int x1, int y1, int thick, uint32_t color) {
    const int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    for (int i = 0; i <= steps; i++) {
        const int x = x0 + (x1 - x0) * i / (steps ? steps : 1), y = y0 + (y1 - y0) * i / (steps ? steps : 1);
        Disc(x, y, thick / 2, color);
    }
}

enum class Button { Cross, Circle, Triangle, Square };

// A PS4 face button symbol in a dark disc; returns the x after it.
int DrawButton(int x, int y, Button b) {
    const int r = 20, cx = x + r, cy = y + r;
    Disc(cx, cy, r, Rgb(10, 13, 24));
    switch (b) {
    case Button::Cross:
        Line(cx - 8, cy - 8, cx + 8, cy + 8, 4, kCrossBlue);
        Line(cx - 8, cy + 8, cx + 8, cy - 8, 4, kCrossBlue);
        break;
    case Button::Circle:
        Ring(cx, cy, 11, 4, kCircleRed);
        break;
    case Button::Triangle:
        Line(cx, cy - 10, cx - 10, cy + 7, 4, kTriangleGreen);
        Line(cx, cy - 10, cx + 10, cy + 7, 4, kTriangleGreen);
        Line(cx - 10, cy + 7, cx + 10, cy + 7, 4, kTriangleGreen);
        break;
    case Button::Square:
        Line(cx - 8, cy - 8, cx + 8, cy - 8, 4, kSquarePink);
        Line(cx + 8, cy - 8, cx + 8, cy + 8, 4, kSquarePink);
        Line(cx + 8, cy + 8, cx - 8, cy + 8, 4, kSquarePink);
        Line(cx - 8, cy + 8, cx - 8, cy - 8, 4, kSquarePink);
        break;
    }
    return x + 2 * r + 10;
}

// A shoulder/menu button label (L1, R1) as a key cap; returns the x after it.
// The logo and the app's name at the top left; returns where they end.
int DrawBrand() {
    constexpr int kLogoX = 80, kLogoY = 34;
    for (int y = 0; y < kLogoSize; y++)
        for (int x = 0; x < kLogoSize; x++) {
            const uint32_t p = kLogoPixels[y * kLogoSize + x];
            uint32_t& dst = g_frame[(kLogoY + y) * kWidth + kLogoX + x];
            dst = Blend(dst, p, p >> 24);
        }
    const int name_x = kLogoX + kLogoSize + 20;
    DrawText(kTitleFont, name_x, 28, "Switch Emu", kWhite);
    const int x = name_x + TextWidth(kTitleFont, "Switch Emu") + 14;
    DrawText(kBold, x, 56, "for PS4", kAccent);
    return x + TextWidth(kBold, "for PS4");
}

int DrawPill(int x, int y, const char* label) {
    const int w = TextWidth(kBold, label) + 24;
    RoundRect(x, y, w, 40, 10, Rgb(10, 13, 24));
    DrawText(kBold, x + 12, y + (40 - kBoldFontHeight) / 2, label, kText);
    return x + w + 10;
}

int Hint(int x, int y, const char* text) {
    return DrawText(kRegular, x, y + (40 - kStatusFontHeight) / 2, text, kText) + 40;
}

uint32_t TileColor(const char* title) {
    static const uint32_t kColors[] = {Rgb(231, 76, 60), Rgb(52, 152, 219), Rgb(46, 204, 113), Rgb(155, 89, 182),
                                       Rgb(241, 196, 15), Rgb(230, 126, 34), Rgb(26, 188, 156), Rgb(236, 64, 122)};
    uint32_t h = 2166136261u;
    for (; *title; title++)
        h = (h ^ uint8_t(*title)) * 16777619u;
    return kColors[h % (sizeof(kColors) / sizeof(kColors[0]))];
}

// Box-filtered downscale of a kLauncherIconSize icon to size x size.
uint32_t* ScaleIcon(const uint32_t* icon, int size) {
    auto* out = static_cast<uint32_t*>(malloc(size_t(size) * size * 4));
    if (!out)
        return nullptr;
    if (size > kLauncherIconSize) { // enlarging (the row layout's tiles): bilinear
        constexpr int kMax = kLauncherIconSize - 1;
        for (int y = 0; y < size; y++) {
            const float fy = std::clamp((float(y) + 0.5f) * kLauncherIconSize / size - 0.5f, 0.0f, float(kMax));
            const int y0 = int(fy), y1 = std::min(y0 + 1, kMax);
            const float ty = fy - float(y0);
            for (int x = 0; x < size; x++) {
                const float fx = std::clamp((float(x) + 0.5f) * kLauncherIconSize / size - 0.5f, 0.0f, float(kMax));
                const int x0 = int(fx), x1 = std::min(x0 + 1, kMax);
                const float tx = fx - float(x0);
                const uint32_t p00 = icon[y0 * kLauncherIconSize + x0], p01 = icon[y0 * kLauncherIconSize + x1],
                               p10 = icon[y1 * kLauncherIconSize + x0], p11 = icon[y1 * kLauncherIconSize + x1];
                const auto mix = [&](uint32_t c00, uint32_t c01, uint32_t c10, uint32_t c11) {
                    const float top = float(c00) + (float(c01) - float(c00)) * tx;
                    const float bottom = float(c10) + (float(c11) - float(c10)) * tx;
                    return uint8_t(top + (bottom - top) * ty + 0.5f);
                };
                out[y * size + x] = Rgb(mix(R(p00), R(p01), R(p10), R(p11)), mix(G(p00), G(p01), G(p10), G(p11)),
                                        mix(B(p00), B(p01), B(p10), B(p11)));
            }
        }
        return out;
    }
    for (int y = 0; y < size; y++) {
        const int y0 = y * kLauncherIconSize / size, y1 = std::max(y0 + 1, (y + 1) * kLauncherIconSize / size);
        for (int x = 0; x < size; x++) {
            const int x0 = x * kLauncherIconSize / size, x1 = std::max(x0 + 1, (x + 1) * kLauncherIconSize / size);
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int sy = y0; sy < y1; sy++)
                for (int sx = x0; sx < x1; sx++) {
                    const uint32_t p = icon[sy * kLauncherIconSize + sx];
                    r += R(p), g += G(p), b += B(p), n++;
                }
            out[y * size + x] = Rgb(uint8_t(r / n), uint8_t(g / n), uint8_t(b / n));
        }
    }
    return out;
}

uint32_t AverageColor(const uint32_t* icon) {
    uint64_t r = 0, g = 0, b = 0;
    for (int i = 0; i < kLauncherIconSize * kLauncherIconSize; i += 7)
        r += R(icon[i]), g += G(icon[i]), b += B(icon[i]);
    const uint64_t n = (kLauncherIconSize * kLauncherIconSize + 6) / 7;
    return Rgb(uint8_t(r / n), uint8_t(g / n), uint8_t(b / n));
}

// Icon (or initials tile) with rounded corners.
void DrawCardImage(GameEntry& g, int x, int y, int size) {
    constexpr int kRadius = 18;
    const uint32_t* pixels = nullptr;
    if (g.icon && size != kCardSize && size != kCardBig && size != kRowTile && size != kRowBig &&
        size != kLoadingCard && g.small_size != size && g.big_size != size) {
        // A size in between (a card growing or shrinking): each pixel straight from the icon,
        // no scaled copy (one a frame would cost far more than the drawing).
        int src_x[kLoadingCard];
        for (int col = 0; col < size && col < kLoadingCard; col++)
            src_x[col] = col * kLauncherIconSize / size;
        for (int row = 0; row < size; row++) {
            const int py = y + row;
            if (py < 0 || py >= kHeight)
                continue;
            const uint32_t* src = g.icon + (row * kLauncherIconSize / size) * kLauncherIconSize;
            const bool corner_row = row < kRadius || row >= size - kRadius;
            uint32_t* dst = g_frame + size_t(py) * kWidth;
            for (int col = 0; col < size && col < kLoadingCard; col++) {
                const int px = x + col;
                if (px < 0 || px >= kWidth)
                    continue;
                const uint32_t c = 0xFF000000u | src[src_x[col]];
                if (corner_row && (col < kRadius || col >= size - kRadius)) {
                    const uint32_t a = RoundCoverage(px, py, x, y, size, size, kRadius);
                    if (a)
                        dst[px] = Blend(dst[px], c, a);
                } else {
                    dst[px] = c;
                }
            }
        }
        return;
    }
    if (g.icon) {
        // Reuse a copy of this size; otherwise replace the smaller-or-equal slot's copy (a layout
        // change or a card becoming selected rescales once).
        if (g.small_size != size && g.big_size != size) {
            const bool use_small = size < g.big_size;
            uint32_t*& slot = use_small ? g.small : g.big;
            free(slot);
            slot = ScaleIcon(g.icon, size);
            (use_small ? g.small_size : g.big_size) = slot ? size : 0;
        }
        pixels = g.small_size == size ? g.small : g.big;
    }
    if (!pixels) {
        RoundRect(x, y, size, size, kRadius, g.accent);
        // A light sheen and the initials of the first two words.
        for (int i = 0; i < size / 2; i++)
            FillBlend(x + kRadius, y + i, size - 2 * kRadius, 1, kWhite, uint32_t(18 * (size / 2 - i) / (size / 2)));
        char initials[3] = {0, 0, 0};
        int n = 0;
        bool word_start = true;
        for (const char* p = g.title; *p && n < 2; p++) {
            const bool alnum = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9');
            if (alnum && word_start)
                initials[n++] = (*p >= 'a' && *p <= 'z') ? char(*p - 32) : *p;
            word_start = !alnum;
        }
        const Font& f = size >= 280 ? kTitleFont : kBold;
        DrawText(f, x + (size - TextWidth(f, initials)) / 2, y + (size - f.height) / 2, initials, kWhite, x + size);
        return;
    }
    // Rows straight across (clipped to the screen); only the rounded corners blend.
    const int col0 = std::max(0, -x), col1 = std::min(size, kWidth - x);
    if (col0 >= col1)
        return;
    for (int row = 0; row < size; row++) {
        const int py = y + row;
        if (py < 0 || py >= kHeight)
            continue;
        uint32_t* dst = g_frame + size_t(py) * kWidth;
        const uint32_t* src = pixels + size_t(row) * size;
        if (row >= kRadius && row < size - kRadius) {
            memcpy(dst + x + col0, src + col0, size_t(col1 - col0) * 4);
            continue;
        }
        const int mid0 = std::max(col0, kRadius), mid1 = std::min(col1, size - kRadius);
        if (mid0 < mid1)
            memcpy(dst + x + mid0, src + mid0, size_t(mid1 - mid0) * 4);
        for (int col = col0; col < col1; col++) {
            if (col == mid0 && mid0 < mid1)
                col = mid1;
            if (col >= col1)
                break;
            const int px = x + col;
            const uint32_t a = RoundCoverage(px, py, x, y, size, size, kRadius);
            if (a)
                dst[px] = Blend(dst[px], src[col], a);
        }
    }
}

// Kept for the loading screen: a card with a shadow, outlined when selected.
void DrawCard(GameEntry& g, int x, int y, int size, bool selected) {
    Shadow(x, y, size, size, 18);
    if (selected)
        RoundRect(x - 6, y - 6, size + 12, size + 12, 24, kWhite);
    DrawCardImage(g, x, y, size);
}
void DrawCard(const GameEntry& g, int x, int y, int size, bool selected) {
    GameEntry copy = g;
    copy.small = copy.big = nullptr; // a temporary: don't cache into it
    copy.small_size = copy.big_size = 0;
    if (copy.icon && size != kCardSize && size != kCardBig) {
        // Arbitrary size (loading screen): nearest-neighbour from the source icon.
        Shadow(x, y, size, size, 22);
        if (selected)
            RoundRect(x - 6, y - 6, size + 12, size + 12, 28, kWhite);
        for (int row = 0; row < size; row++)
            for (int col = 0; col < size; col++) {
                const int px = x + col, py = y + row;
                if (px < 0 || px >= kWidth || py < 0 || py >= kHeight)
                    continue;
                const uint32_t a = RoundCoverage(px, py, x, y, size, size, 22);
                const uint32_t c = copy.icon[(row * kLauncherIconSize / size) * kLauncherIconSize + col * kLauncherIconSize / size];
                if (a)
                    g_frame[py * kWidth + px] = Blend(g_frame[py * kWidth + px], c, a);
            }
        return;
    }
    DrawCard(copy, x, y, size, selected);
    free(copy.small);
    free(copy.big);
}

// Text shortened with "..." to fit max_w.
void Ellipsize(const Font& f, char* text, size_t size, int max_w) {
    if (TextWidth(f, text) <= max_w)
        return;
    size_t n = strlen(text);
    while (n > 0) {
        text[--n] = 0;
        if (TextWidth(f, text) + TextWidth(f, "...") <= max_w)
            break;
    }
    strncat(text, "...", size - strlen(text) - 1);
}

// An on/off switch, right-aligned at x_right.
// A switch; pos slides the knob from off (0) to on (1).
void Toggle(int x_right, int y, float pos, bool selected) {
    constexpr int kW = 66, kH = 36;
    const int x = x_right - kW;
    RoundRect(x, y, kW, kH, kH / 2, Mix(Rgb(58, 66, 88), kAccent, pos));
    const int knob = x + kH / 2 + int(float(kW - kH) * Ease(pos));
    Disc(knob, y + kH / 2, kH / 2 - 4, selected ? kWhite : Mix(Rgb(200, 205, 215), kWhite, pos));
}

void DrawToast(double now) {
    char text[kLineLen];
    CopyText(text, sizeof(text), g_toast);
    if (!text[0] || now >= g_toast_until)
        return;
    const double left = g_toast_until - now;
    const uint32_t alpha = left < 0.4 ? uint32_t(left / 0.4 * 230) : 230;
    const int w = TextWidth(kBold, text) + 70, h = 60, x = (kWidth - w) / 2, y = 112;
    if (alpha > 200)
        Shadow(x, y, w, h, h / 2);
    RoundRect(x, y, w, h, h / 2, Rgb(236, 240, 248), alpha);
    DrawText(kBold, x + 35, y + (h - kBoldFontHeight) / 2, text, Rgb(14, 20, 34));
}

// Row layout of the games tab: one row of large tiles that scrolls sideways, the selected game's
// title above it, and round shortcuts below. Returns true while animating.
bool DrawGameRow(int game_sel, int game_count, int shortcut, float k) {
    constexpr int kTile = kRowTile, kBig = kRowBig, kGap = 30, kPitch = kTile + kGap, kLeft = 100, kTop = 250;
    constexpr int kVisible = (kWidth - 2 * kLeft + kGap) / kPitch;
    bool moving = false;
    // Scrolls only when the selection would leave the screen, by as little as needed.
    static int first = 0;
    const int max_first = std::max(0, game_count - kVisible);
    if (game_sel < first)
        first = game_sel;
    if (game_sel > first + kVisible - 1)
        first = game_sel - kVisible + 1;
    first = std::clamp(first, 0, max_first);
    const float target = float(first * kPitch);
    moving |= Approach(g_anim.row_scroll, target, k);
    const float sel_x = float(kLeft + game_sel * kPitch - (kBig - kTile) / 2);
    if (g_anim.ring_x < 0)
        g_anim.ring_x = sel_x;
    moving |= Approach(g_anim.ring_x, sel_x, k);
    const int scroll = int(g_anim.row_scroll);
    const bool on_games = shortcut < 0;
    for (int i = 0; i < game_count; i++)
        moving |= ApproachUnit(g_anim.grow[i], i == game_sel ? 1.0f : 0.0f, k);

    Lock();
    for (int i = 0; i < game_count; i++) {
        const int x = kLeft + i * kPitch - scroll;
        if (i == game_sel || x < kLeft - kGap || x > kWidth)
            continue; // the edge of the screen shows there is more to the right; the left stays clear
        const int size = kTile + int(float(kBig - kTile) * Ease(g_anim.grow[i]));
        const int off = (size - kTile) / 2;
        Shadow(x - off, kTop - off, size, size, 22);
        DrawCardImage(g_games[i], x - off, kTop - off, size);
    }
    GameEntry& g = g_games[game_sel];
    const int big = kTile + int(float(kBig - kTile) * Ease(g_anim.grow[game_sel]));
    const int boff = (big - kTile) / 2;
    const int sx = kLeft + game_sel * kPitch - scroll - boff, sy = kTop - boff;
    Shadow(sx, sy, big, big, 26);
    if (on_games) {
        const int rcx = int(g_anim.ring_x) - scroll + kBig / 2, rcy = kTop + kTile / 2;
        RoundRect(rcx - big / 2 - 9, rcy - big / 2 - 9, big + 18, big + 18, 32, kAccent);
        RoundRect(rcx - big / 2 - 4, rcy - big / 2 - 4, big + 8, big + 8, 27, Rgb(14, 20, 34));
    }
    DrawCardImage(g, sx, sy, big);
    g_anim.sel_x = sx, g_anim.sel_y = sy, g_anim.sel_size = big;
    char title[96], detail[128];
    CopyText(title, sizeof(title), g.title);
    CopyText(detail, sizeof(detail), g.detail);
    Unlock();
    // The selected game's title above it, kept on screen.
    Ellipsize(kBold, title, sizeof(title), 900);
    const int tw = TextWidth(kBold, title);
    const int tx = std::clamp(sx + big / 2 - tw / 2, kLeft, kWidth - kLeft - tw);
    DrawText(kBold, tx, kTop - (kBig - kTile) / 2 - 62, title, on_games ? kAccent : kDim);

    // Shortcuts: round buttons, centred, with the selected one's name under it.
    static const char* const kNames[kShortcutCount] = {"Settings", "Install updates and DLC", "Back up saves"};
    constexpr int kR = 46, kStep = 136, kY = 730;
    const int x0 = kWidth / 2 - (kShortcutCount - 1) * kStep / 2;
    for (int i = 0; i < kShortcutCount; i++) {
        const int cx = x0 + i * kStep;
        moving |= ApproachUnit(g_anim.shortcut_grow[i], i == shortcut ? 1.0f : 0.0f, k * 1.3f);
        const float sg = Ease(g_anim.shortcut_grow[i]);
        const bool sel = sg > 0.5f;
        if (sg > 0.02f)
            Disc(cx, kY, kR + int(8 * sg), Mix(Rgb(14, 20, 34), kAccent, sg));
        const uint32_t fill = Mix(Rgb(30, 40, 64), Rgb(48, 62, 96), sg);
        Disc(cx, kY, kR, fill);
        const uint32_t ink = Mix(Rgb(190, 202, 225), kWhite, sg);
        switch (i) {
        case kShortcutSettings: // a gear
            for (int t = 0; t < 8; t++) {
                const float a = float(t) * 3.14159265f / 4;
                Disc(cx + int(std::cos(a) * 20), kY + int(std::sin(a) * 20), 6, ink);
            }
            Disc(cx, kY, 19, ink);
            Disc(cx, kY, 8, fill);
            break;
        case kShortcutInstall: // an arrow into a tray
            Line(cx, kY - 24, cx, kY + 8, 6, ink);
            Line(cx - 13, kY - 5, cx, kY + 9, 6, ink);
            Line(cx + 13, kY - 5, cx, kY + 9, 6, ink);
            Line(cx - 22, kY + 22, cx + 22, kY + 22, 6, ink);
            break;
        default: { // a save card
            const uint32_t bg = fill;
            RoundRect(cx - 21, kY - 24, 42, 48, 7, ink);
            RoundRect(cx - 13, kY - 24, 24, 15, 3, bg);
            RoundRect(cx - 13, kY + 4, 26, 14, 3, bg);
            break;
        }
        }
    }
    if (!on_games) {
        const int i = std::clamp(shortcut, 0, kShortcutCount - 1);
        DrawText(kBold, x0 + i * kStep - TextWidth(kBold, kNames[i]) / 2, kY + kR + 22, kNames[i],
                 Mix(Rgb(14, 20, 34), kWhite, Ease(g_anim.shortcut_grow[i])));
    }

    // Info bar: the selected game's details.
    if (on_games) {
        RoundRect(80, 850, kWidth - 160, 64, 20, Rgb(10, 13, 24), 150);
        DrawText(kRegular, 108, 850 + (64 - kStatusFontHeight) / 2, detail, kDim, kWidth - 260);
        char count_text[48];
        snprintf(count_text, sizeof(count_text), "%d of %d", game_sel + 1, game_count);
        DrawText(kWidth - 108 - TextWidth(kRegular, count_text), 850 + (64 - kStatusFontHeight) / 2, count_text,
                 kDim);
    }
    return moving;
}

// A game was chosen: its card moves and grows into its place on the loading screen (DrawLoading)
// while the launcher fades out behind it. Returns true until done.
bool DrawLaunch(double now) {
    if (!g_launch_snapped)
        g_launch_snapped = SnapshotFrame();
    const float t = Ease(float((now - g_launch_t0) / 0.32));
    Lock();
    const int sel = std::clamp(g_game_sel, 0, std::max(0, g_game_count - 1));
    const uint32_t accent = g_game_count > 0 ? g_games[sel].accent : kAccent;
    char title[96] = "";
    if (g_game_count > 0)
        CopyText(title, sizeof(title), g_games[sel].title);
    Unlock();
    DrawBackground(accent);
    DrawBrand();
    BlendPrev(0, uint32_t((1.0f - t) * 256.0f));
    // DrawLoading's card: 360 at (140, 300).
    const float from_x = float(g_anim.sel_x), from_y = float(g_anim.sel_y), from_size = float(std::max(g_anim.sel_size, 1));
    const int size = int(from_size + (360.0f - from_size) * t);
    const int x = int(from_x + (140.0f - from_x) * t), y = int(from_y + (300.0f - from_y) * t);
    Shadow(x, y, size, size, 24);
    if (g_game_count > 0) {
        Lock();
        DrawCardImage(g_games[sel], x, y, size);
        Unlock();
    }
    DrawText(kTitleFont, 140 + 360 + 90, 300 + 24, title, Mix(Rgb(14, 20, 34), kWhite, t), kWidth - 80);
    return t < 1.0f;
}

void DrawLauncher() {
    const double now = Elapsed();
    const float dt = g_anim.last > 0 ? float(std::min(now - g_anim.last, 0.1)) : 1.0f;
    g_anim.last = now;
    const float k = 1.0f - std::exp(-dt * 16.0f); // easing toward targets
    bool moving = false;
    if (g_launch_t0 >= 0) {
        g_animating = DrawLaunch(now);
        return;
    }

    Lock();
    const int tab = g_tab, game_sel = g_game_sel, setting_sel = g_setting_sel, game_count = g_game_count,
              setting_count = g_setting_count, install_count = g_install_count;
    const bool has_pad = g_has_pad, scope_switchable = g_scope_switchable;
    const int layout = g_layout, shortcut = g_shortcut;
    char scope[kLineLen];
    CopyText(scope, sizeof(scope), g_scope);
    const uint32_t accent = game_count > 0 && tab == kLauncherGames ? g_games[game_sel].accent : kAccent;
    Unlock();

    // A new page (tab, layout, or the settings' scope) cross-fades from the last frame shown.
    if (g_anim.last_tab >= 0 && (tab != g_anim.last_tab || layout != g_anim.last_layout ||
                                 (tab == kLauncherSettings && strcmp(scope, g_anim.last_scope) != 0))) {
        if (SnapshotFrame())
            g_anim.page_fade = 1;
    }
    g_anim.last_tab = tab, g_anim.last_layout = layout;
    CopyText(g_anim.last_scope, sizeof(g_anim.last_scope), scope);

    // Background tint follows the selected game.
    moving |= Approach(g_anim.tint_r, R(accent), k);
    moving |= Approach(g_anim.tint_g, G(accent), k);
    moving |= Approach(g_anim.tint_b, B(accent), k);
    DrawBackground();

    const auto draw_header = [&] {
        const int brand_end = DrawBrand();
        {
            const char* tabs[] = {"Games", "Settings"};
            int tx = std::max(470, brand_end + 48);
            const int px = DrawPill(tx, 48, "L1");
            tx = px + 6;
            int xs[2], ws[2];
            const int seg_x = tx, seg_y = 44, seg_h = 48;
            int total = 0;
            for (int i = 0; i < 2; i++)
                ws[i] = TextWidth(kBold, tabs[i]) + 48, total += ws[i];
            RoundRect(seg_x, seg_y, total + 8, seg_h, seg_h / 2, Rgb(10, 13, 24), 200);
            int cx = seg_x + 4;
            for (int i = 0; i < 2; i++)
                xs[i] = cx, cx += ws[i];
            if (g_anim.tab_x < 0)
                g_anim.tab_x = float(xs[tab]), g_anim.tab_w = float(ws[tab]);
            moving |= Approach(g_anim.tab_x, float(xs[tab]), k);
            moving |= Approach(g_anim.tab_w, float(ws[tab]), k);
            RoundRect(int(g_anim.tab_x), seg_y + 4, int(g_anim.tab_w), seg_h - 8, (seg_h - 8) / 2, Rgb(236, 240, 248));
            for (int i = 0; i < 2; i++)
                DrawText(kBold, xs[i] + 24, seg_y + (seg_h - kBoldFontHeight) / 2, tabs[i], i == tab ? Rgb(14, 20, 34) : kText);
            DrawPill(seg_x + total + 18, 48, "R1");
        }
        const time_t clock_now = time(nullptr);
        if (const tm* local = localtime(&clock_now)) {
            char clock_text[16];
            snprintf(clock_text, sizeof(clock_text), "%d:%02d", local->tm_hour, local->tm_min);
            DrawText(kBold, kWidth - 80 - TextWidth(kBold, clock_text), 54, clock_text, kText);
        }
    };
    if (tab == kLauncherGames) {
        if (game_count == 0) {
            DrawText(kTitleFont, 100, 340, "No games yet", kWhite);
            DrawText(100, 450, "Copy games (.nsp, .xci) or homebrew (.nro) to /data/switch-emu/games over FTP,", kText);
            DrawText(100, 492, "then restart the app. Keys go in /data/switch-emu/keys, firmware in", kText);
            DrawText(100, 534, "/data/switch-emu/nand/system/Contents/registered.", kText);
        } else if (layout == kLayoutRow) {
            moving |= DrawGameRow(game_sel, game_count, shortcut, k);
        } else {
            constexpr int kCols = kLauncherColumns, kGapX = 62, kTitleH = 44, kGapY = 24, kTop = 140, kRows = 3,
                          kRowH = kCardSize + kTitleH + kGapY, kLeft = 100;
            const int rows = (game_count + kCols - 1) / kCols;
            const int sel_row = game_sel / kCols, sel_col = game_sel % kCols;
            const int first_row = rows <= kRows ? 0 : std::clamp(sel_row - 1, 0, rows - kRows);
            moving |= Approach(g_anim.grid_scroll, float(first_row * kRowH), k);
            const int scroll = int(g_anim.grid_scroll);
            // Selection ring target.
            const float ring_x = float(kLeft + sel_col * (kCardSize + kGapX) - (kCardBig - kCardSize) / 2);
            const float ring_y = float(kTop + sel_row * kRowH - (kCardBig - kCardSize) / 2);
            if (g_anim.ring_x < 0)
                g_anim.ring_x = ring_x, g_anim.ring_y = ring_y;
            moving |= Approach(g_anim.ring_x, ring_x, k);
            moving |= Approach(g_anim.ring_y, ring_y, k);
            const int clip_top = kTop - 24, clip_bottom = kTop + kRows * kRowH - 10;
            for (int i = 0; i < game_count; i++)
                moving |= ApproachUnit(g_anim.grow[i], i == game_sel ? 1.0f : 0.0f, k);
            Lock();
            for (int i = 0; i < game_count; i++) {
                const int r = i / kCols, c = i % kCols;
                const int x = kLeft + c * (kCardSize + kGapX), y = kTop + r * kRowH - scroll;
                if (y + kCardSize + kTitleH < clip_top || y > clip_bottom)
                    continue;
                const bool sel = i == game_sel;
                if (sel)
                    continue; // drawn last, on top
                const int size = kCardSize + int(float(kCardBig - kCardSize) * Ease(g_anim.grow[i]));
                const int off = (size - kCardSize) / 2;
                Shadow(x - off, y - off, size, size, 18);
                DrawCardImage(g_games[i], x - off, y - off, size);
                char title[96];
                CopyText(title, sizeof(title), g_games[i].title);
                Ellipsize(kRegular, title, sizeof(title), kCardSize + kGapX - 14);
                DrawText(kRegular, x, y + kCardSize + 12, title, kDim, x + kCardSize + kGapX);
            }
            // The selected card grows in place; a ring glides over to it.
            GameEntry& g = g_games[game_sel];
            const int size = kCardSize + int(float(kCardBig - kCardSize) * Ease(g_anim.grow[game_sel]));
            const int off = (size - kCardSize) / 2;
            const int slot_x = kLeft + sel_col * (kCardSize + kGapX), slot_y = kTop + sel_row * kRowH - scroll;
            const int sx = slot_x - off, sy = slot_y - off;
            const int ring_cx = int(g_anim.ring_x) + kCardBig / 2, ring_cy = int(g_anim.ring_y) - scroll + kCardBig / 2;
            Shadow(sx, sy, size, size, 22);
            RoundRect(ring_cx - size / 2 - 7, ring_cy - size / 2 - 7, size + 14, size + 14, 28, kWhite);
            RoundRect(ring_cx - size / 2 - 3, ring_cy - size / 2 - 3, size + 6, size + 6, 24, Rgb(14, 20, 34));
            DrawCardImage(g, sx, sy, size);
            g_anim.sel_x = sx, g_anim.sel_y = sy, g_anim.sel_size = size;
            char title[96];
            CopyText(title, sizeof(title), g.title);
            Ellipsize(kBold, title, sizeof(title), kCardBig + kGapX / 2 - 8);
            DrawText(kBold, sx, sy + size + 10, title, kWhite, sx + kCardBig + kGapX);
            char detail[128];
            CopyText(detail, sizeof(detail), g.detail);
            char full_title[96];
            CopyText(full_title, sizeof(full_title), g.title);
            Unlock();
            // Clip what scrolled past the grid's top and bottom.
            RepaintBackground(0, kTop - 22);
            RepaintBackground(clip_bottom + 8, 850);
            if (first_row > 0)
                DrawText(kBold, kWidth - 64, kTop + 60, "^", kDim);
            if (first_row + kRows < rows)
                DrawText(kBold, kWidth - 64, kTop + (kRows - 1) * kRowH + 80, "v", kDim);
            // Info bar: the selected game's full title and details.
            RoundRect(80, 850, kWidth - 160, 64, 20, Rgb(10, 13, 24), 150);
            DrawText(kBold, 108, 850 + (64 - kBoldFontHeight) / 2, full_title, kWhite, kWidth / 2);
            DrawText(kRegular, 108 + TextWidth(kBold, full_title) + 26, 850 + (64 - kStatusFontHeight) / 2, detail, kDim,
                     kWidth - 260);
            char count_text[48];
            snprintf(count_text, sizeof(count_text), "%d of %d", game_sel + 1, game_count);
            DrawText(kWidth - 108 - TextWidth(kRegular, count_text), 850 + (64 - kStatusFontHeight) / 2, count_text,
                     kDim);
        }
        int hx = 100;
        const int hy = 936;
        if (game_count > 0 && layout == kLayoutRow && shortcut >= 0) {
            hx = DrawButton(hx, hy, Button::Cross);
            hx = Hint(hx, hy, "Open");
            hx = DrawButton(hx, hy, Button::Circle);
            hx = Hint(hx, hy, "Back to games");
        } else if (game_count > 0) {
            hx = DrawButton(hx, hy, Button::Cross);
            hx = Hint(hx, hy, "Play");
            hx = DrawButton(hx, hy, Button::Square);
            hx = Hint(hx, hy, "Game settings");
        }
        if (install_count > 0) {
            hx = DrawButton(hx, hy, Button::Triangle);
            char text[64];
            snprintf(text, sizeof(text), "Install %d update/DLC file%s", install_count, install_count == 1 ? "" : "s");
            hx = Hint(hx, hy, text);
        }
        hx = DrawPill(hx, hy, "R1");
        Hint(hx, hy, "Settings");
    } else {
        // Settings: grouped rows on the left, the selected row's help on the right.
        constexpr int kRowH = 56, kGroupH = 50, kListX = 100, kListW = 1000, kTop = 236, kBottom = 892;
        // Scope chip.
        DrawText(kRegular, kListX, 132, "Settings for", kDim);
        {
            const int cx = kListX + TextWidth(kRegular, "Settings for") + 16;
            const int w = TextWidth(kBold, scope) + 36;
            RoundRect(cx, 124, w, 46, 23, Rgb(236, 240, 248));
            DrawText(kBold, cx + 18, 124 + (46 - kBoldFontHeight) / 2, scope, Rgb(14, 20, 34));
        }
        // Layout: y of each row (with group headings) in list space.
        int ys[kMaxSettings], total = 0;
        char last_group[32] = "";
        Lock();
        for (int i = 0; i < setting_count; i++) {
            if (g_settings[i].group[0] && strcmp(g_settings[i].group, last_group) != 0) {
                total += kGroupH;
                memcpy(last_group, g_settings[i].group, sizeof(last_group));
            }
            ys[i] = total;
            total += kRowH;
        }
        const int view = kBottom - kTop;
        const int target = setting_count == 0 ? 0
                                               : std::clamp(ys[setting_sel] - view / 2 + kRowH, 0, std::max(0, total - view));
        moving |= Approach(g_anim.list_scroll, float(target), k);
        const int scroll = int(g_anim.list_scroll);
        // The highlight bar glides between rows (behind them).
        if (setting_count > 0) {
            if (g_anim.bar_y < 0)
                g_anim.bar_y = float(ys[setting_sel]);
            moving |= Approach(g_anim.bar_y, float(ys[setting_sel]), k * 1.3f);
            const int by = kTop + int(g_anim.bar_y) - scroll;
            RoundRect(kListX - 10, by, kListW + 20, kRowH - 6, 16, Rgb(236, 240, 248), 34);
            RoundRect(kListX - 10, by + 10, 6, kRowH - 26, 3, kAccent);
        }
        last_group[0] = 0;
        for (int i = 0; i < setting_count; i++) {
            const SettingEntry& e = g_settings[i];
            const int y = kTop + ys[i] - scroll;
            if (e.group[0] && strcmp(e.group, last_group) != 0) {
                memcpy(last_group, e.group, sizeof(last_group));
                const int gy = y - kGroupH + 14;
                if (gy > kTop - 30 && gy < kBottom)
                    DrawText(kBold, kListX + 4, gy, e.group, kAccent);
            }
            if (y < kTop - 10 || y + kRowH > kBottom + 10)
                continue;
            const bool sel = i == setting_sel;
            DrawText(kRegular, kListX + 22, y + (kRowH - 6 - kStatusFontHeight) / 2, e.label, sel ? kWhite : kText);
            const int vr = kListX + kListW - 20;
            if (e.kind == kSettingToggle) {
                const float on = strcmp(e.value, "On") == 0 ? 1.0f : 0.0f;
                if (!g_anim.toggle_known[i])
                    g_anim.toggle[i] = on, g_anim.toggle_known[i] = true;
                moving |= ApproachUnit(g_anim.toggle[i], on, k * 1.4f);
                if (std::fabs(g_anim.toggle[i] - on) < 0.02f)
                    g_anim.toggle[i] = on;
                Toggle(vr, y + (kRowH - 6 - 36) / 2, g_anim.toggle[i], sel);
            } else {
                char value[110];
                if (e.kind == kSettingChoice)
                    snprintf(value, sizeof(value), "<   %s   >", e.value);
                else
                    snprintf(value, sizeof(value), "%s", e.value);
                DrawText(kBold, vr - TextWidth(kBold, value), y + (kRowH - 6 - kBoldFontHeight) / 2, value,
                         sel ? kWhite : (e.kind == kSettingAction ? kAccent : Rgb(170, 200, 240)));
            }
        }
        SettingEntry selected{};
        if (setting_sel >= 0 && setting_sel < setting_count)
            selected = g_settings[setting_sel];
        Unlock();
        // Clip what scrolled past the list's top and bottom, then the scope chip again.
        RepaintBackground(0, kTop - 6);
        RepaintBackground(kBottom, kHeight);
        DrawText(kRegular, kListX, 132, "Settings for", kDim);
        {
            const int cx = kListX + TextWidth(kRegular, "Settings for") + 16;
            const int w = TextWidth(kBold, scope) + 36;
            RoundRect(cx, 124, w, 46, 23, Rgb(236, 240, 248));
            DrawText(kBold, cx + 18, 124 + (46 - kBoldFontHeight) / 2, scope, Rgb(14, 20, 34));
        }
        // Help card.
        const int px = 1180, pw = kWidth - px - 80, py = kTop;
        Shadow(px, py, pw, 330, 24);
        RoundRect(px, py, pw, 330, 24, kCard);
        if (setting_sel != g_anim.help_row)
            g_anim.help_row = setting_sel, g_anim.help_fade = 0;
        moving |= ApproachUnit(g_anim.help_fade, 1.0f, k);
        const float hf = Ease(g_anim.help_fade);
        if (selected.group[0])
            DrawText(kRegular, px + 32, py + 26, selected.group, Mix(kCard, kAccent, hf));
        DrawText(kBold, px + 32, py + 26 + kStatusFontHeight + 4, selected.label, Mix(kCard, kWhite, hf), px + pw - 20);
        int y = py + 26 + kStatusFontHeight + kBoldFontHeight + 24;
        const char* p = selected.help;
        char line[kLineLen];
        while (*p && y < py + 310) {
            int len = 0, last_space = -1;
            while (p[len] && len < kLineLen - 1) {
                line[len] = p[len];
                line[len + 1] = 0;
                if (TextWidth(kRegular, line) > pw - 64)
                    break;
                if (p[len] == ' ')
                    last_space = len;
                len++;
            }
            if (p[len] && last_space > 0)
                len = last_space + 1;
            memcpy(line, p, size_t(len));
            line[len] = 0;
            DrawText(px + 32, y, line, Mix(kCard, kText, hf), px + pw - 20);
            y += kStatusFontHeight + 4;
            p += len;
        }
        int hx = 100;
        const int hy = 936;
        hx = DrawButton(hx, hy, Button::Cross);
        hx = Hint(hx, hy, "Change");
        if (scope_switchable) {
            hx = DrawButton(hx, hy, Button::Square);
            hx = Hint(hx, hy, "All games / this game");
        }
        hx = DrawButton(hx, hy, Button::Circle);
        hx = Hint(hx, hy, "Back");
        hx = DrawPill(hx, hy, "L1");
        Hint(hx, hy, "Games");
    }

    draw_header();
    DrawToast(now);
    moving |= g_toast[0] && now < g_toast_until;

    if (!has_pad)
        DrawText(kWidth - 80 - TextWidth(kRegular, "Connect a controller"), kHeight - 66, "Connect a controller", kBad);
    if (g_anim.page_fade > 0) { // below the header, which stays put
        BlendPrev(104, uint32_t(Ease(g_anim.page_fade) * 256.0f));
        g_anim.page_fade = std::max(0.0f, g_anim.page_fade - dt / 0.22f);
        moving = true;
    }
    g_animating = moving;
}

// Loading screen (drawn into the Vulkan swap chain's first image until the game's first frame).
char g_loading_title[96] = "";
const uint32_t* g_loading_icon = nullptr;

// Loading screen state (ps4_loading_*): what is really happening between the launcher and the
// game's first frame, so nothing pretends to know how long the unknown part takes.
struct LoadingState {
    std::atomic<int> stage{0};          // 0 starting the graphics, 1 shaders from the cache, 2 starting the game
    std::atomic<unsigned> cache_done{0}, cache_total{0};
    std::atomic<unsigned long long> shaders_at_start{0};
    double shown = NAN, stage_time = NAN; // Elapsed() when first drawn / when the stage began (NaN: not yet)
} g_load;

void DrawSpinner(int cx, int cy, double t); // below

// A green disc with a check mark.
void DrawCheck(int cx, int cy) {
    Disc(cx, cy, 15, kGood);
    Line(cx - 7, cy + 1, cx - 2, cy + 6, 4, Rgb(14, 20, 34));
    Line(cx - 2, cy + 6, cx + 8, cy - 6, 4, Rgb(14, 20, 34));
}

const char* const kLoadingTips[] = {
    "Hold L3 + R3 (press both sticks) for 1 second in a game to open the Home menu.",
    "Square on a game in the launcher gives it its own settings (resolution, docked or handheld).",
    "Shaders built now are saved: the next start of this game loads them from the cache.",
    "Async shaders (Settings > Performance) keeps games moving while new shaders build.",
    "Updates and DLC go in /data/switch-emu/install - Triangle in the launcher installs them.",
    "The Home menu's performance overlay shows the frame rate and where the time goes.",
};

void DrawLoading(const char* /*progress*/) {
    const double now = Elapsed();
    if (std::isnan(g_load.shown))
        g_load.shown = now;
    if (std::isnan(g_load.stage_time))
        g_load.stage_time = now;
    GameEntry g{};
    CopyText(g.title, sizeof(g.title), g_loading_title[0] ? g_loading_title : "Starting");
    g.icon = g_loading_icon;
    const uint32_t accent = g.icon ? AverageColor(g.icon) : TileColor(g.title);
    DrawBackground(accent);
    DrawBrand();

    // The game's card, breathing gently while it loads.
    constexpr int kIcon = 360, kX = 140, kY = 260;
    const float breathe = 0.5f + 0.5f * float(std::sin(now * 2.2));
    g.accent = accent;
    RoundRect(kX - 14, kY - 14, kIcon + 28, kIcon + 28, 34, Mix(accent, kWhite, 0.35f), uint32_t(40 + 50 * breathe));
    DrawCard(static_cast<const GameEntry&>(g), kX, kY, kIcon, false);

    const int tx = kX + kIcon + 90, right = kWidth - 140;
    DrawText(kTitleFont, tx, kY + 4, g.title, kWhite, right);

    const int stage = g_load.stage.load();
    const unsigned done = g_load.cache_done.load(), total = g_load.cache_total.load();
    const unsigned long long built = ps4_pipelines_built.load() - g_load.shaders_at_start.load();
    char detail[96];
    int y = kY + kTitleFontHeight + 46;
    const auto step = [&](int index, const char* label, const char* info) {
        const int cy = y + 18;
        if (stage > index)
            DrawCheck(tx + 15, cy);
        else if (stage == index)
            DrawSpinner(tx + 15, cy, now);
        else
            Ring(tx + 15, cy, 13, 3, Rgb(70, 82, 110));
        DrawText(kBold, tx + 50, y + (36 - kBoldFontHeight) / 2, label, stage >= index ? kWhite : kDim, right);
        if (info && info[0])
            DrawText(right - TextWidth(kRegular, info), y + (36 - kStatusFontHeight) / 2, info,
                     stage == index ? kText : kDim);
        y += 62;
    };

    step(0, "Starting the graphics", "");
    if (stage <= 1 && total > 0)
        std::snprintf(detail, sizeof(detail), "%u of %u", std::min(done, total), total);
    else if (stage >= 2)
        std::snprintf(detail, sizeof(detail), total > 0 ? "%u loaded" : "none yet - first start", total);
    else
        detail[0] = 0;
    step(1, "Loading shaders from the cache", detail);
    if (stage == 1 && total > 0) {
        // A real fill: shaders loaded so far out of those in the cache.
        const int bx = tx + 50, bw = right - bx, bh = 10, by = y - 18;
        RoundRect(bx, by, bw, bh, bh / 2, Rgb(255, 255, 255), 36);
        RoundRect(bx, by, std::max(bh, int(int64_t(bw) * std::min(done, total) / total)), bh, bh / 2, kAccent);
        y += 14;
    }
    if (stage >= 2) {
        const int secs = int(now - g_load.stage_time);
        std::snprintf(detail, sizeof(detail), "%d:%02d%s", secs / 60, secs % 60, "");
    } else {
        detail[0] = 0;
    }
    step(2, "Starting the game", detail);
    if (stage == 2) {
        // No bar here: how long a game takes to boot isn't known. What it is doing is.
        if (built > 0)
            std::snprintf(detail, sizeof(detail), "Building new shaders: %llu so far (saved for next time)", built);
        else
            std::snprintf(detail, sizeof(detail), "The game is loading its data");
        DrawText(tx + 50, y - 16, detail, kDim, right);
        y += 30;
    }

    // A tip, changing every 7 seconds, fading between them.
    const double tip_time = (now - g_load.shown) / 7.0;
    const int tip = int(tip_time) % int(sizeof(kLoadingTips) / sizeof(kLoadingTips[0]));
    const double phase = tip_time - std::floor(tip_time);
    const float fade = float(std::min({1.0, tip_time < 1 ? 1.0 : phase * 14.0, (1.0 - phase) * 14.0}));
    RoundRect(tx, 900, right - tx, 64, 16, Rgb(10, 13, 24), 150);
    DrawText(kBold, tx + 24, 900 + (64 - kBoldFontHeight) / 2, "Tip", kAccent);
    DrawText(tx + 84, 900 + (64 - kStatusFontHeight) / 2, kLoadingTips[tip], Mix(Rgb(10, 13, 24), kText, fade), right - 20);
}

// In-game Home menu (yuzu_cmd): drawn over a darkened copy of the game's last frame.
uint32_t* g_pause_bg = nullptr;

// Home menu animation: the panel rises in when opened, the highlight glides between items.
struct PauseAnim {
    double opened = -1, last = 0;
    float highlight = -1;
} g_pause_anim;

bool PauseAnimating() {
    const double now = Elapsed();
    return now - g_pause_anim.opened < 0.25 || g_pause_anim.highlight < 0 ||
           std::fabs(g_pause_anim.highlight - std::floor(g_pause_anim.highlight + 0.5f)) > 0.01f;
}

void DrawPause(const Ps4HomeMenu& m) {
    if (g_pause_bg)
        memcpy(g_frame, g_pause_bg, kFrameBytes);
    else
        DrawBackground();
    const double now = Elapsed();
    const float dt = float(std::min(now - g_pause_anim.last, 0.1));
    g_pause_anim.last = now;
    const float open = Ease(float((now - g_pause_anim.opened) / 0.22));
    if (g_pause_anim.highlight < 0)
        g_pause_anim.highlight = float(m.selected);
    ApproachUnit(g_pause_anim.highlight, float(m.selected), 1.0f - std::exp(-dt * 18.0f));
    constexpr int kW = 820, kH = 610;
    const int x = (kWidth - kW) / 2, y = (kHeight - kH) / 2 + int((1.0f - open) * 40.0f);
    Shadow(x, y, kW, kH, 32);
    RoundRect(x, y, kW, kH, 32, kCard, uint32_t(245 * open));
    DrawText(kTitleFont, x + 50, y + 34, m.title ? m.title : "Paused", kWhite, x + kW - 40);
    DrawText(x + 50, y + 34 + kTitleFontHeight + 4, m.stats ? m.stats : "", kDim, x + kW - 40);
    const int list_y = y + 34 + kTitleFontHeight + kStatusFontHeight + 36;
    const float hl = g_pause_anim.highlight;
    RoundRect(x + 30, list_y + int(hl * 72.0f), kW - 60, 60, 18, Rgb(236, 240, 248));
    for (int i = 0; i < m.count; i++) {
        const int ry = list_y + i * 72;
        // Dark text where the highlight covers the item, light elsewhere, blending as it passes.
        const float cover = std::clamp(1.0f - std::fabs(hl - float(i)), 0.0f, 1.0f);
        DrawText(kBold, x + 60, ry + (60 - kBoldFontHeight) / 2, m.items[i], Mix(kText, Rgb(14, 20, 34), cover),
                 x + kW - 40);
    }
    int hx = x + 50;
    const int hy = y + kH - 74;
    hx = DrawButton(hx, hy, Button::Cross);
    hx = Hint(hx, hy, "Select");
    hx = DrawButton(hx, hy, Button::Circle);
    Hint(hx, hy, "Resume");
}

// Everything shown while no game picture is: benchmark runs and their results, installing
// updates and DLC, reading game info, programs run without graphics, errors. What it is comes from
// the current stage text (ps4_status_set_stage); the right card shows a watched file (benchmark
// results, test.nro's output) when there is one, the activity card the latest trace lines.
struct StatusView {
    char title[96] = "";
    char detail[kLineLen] = "";
    float progress = -1;  // 0..1, or -1: none
    bool error = false, finished = false;
};

// "a/b" or "a of b" in the text as a fraction (the first one found); -1 if none.
float ParseFraction(const char* text, int* a_out = nullptr, int* b_out = nullptr) {
    for (const char* p = text; *p; p++) {
        if (*p < '0' || *p > '9' || (p > text && p[-1] >= '0' && p[-1] <= '9'))
            continue;
        char* end = nullptr;
        const long a = strtol(p, &end, 10);
        const char* q = end;
        if (*q == '/')
            q++;
        else if (strncmp(q, " of ", 4) == 0)
            q += 4;
        else
            continue;
        if (*q < '0' || *q > '9')
            continue;
        const long b = strtol(q, nullptr, 10);
        if (b <= 0 || a < 0 || a > b)
            continue;
        if (a_out)
            *a_out = int(a);
        if (b_out)
            *b_out = int(b);
        return float(a) / float(b);
    }
    return -1;
}

StatusView ReadStatus(const char* stage) {
    StatusView v;
    const auto starts = [&](const char* prefix) { return strncmp(stage, prefix, strlen(prefix)) == 0; };
    const auto after = [&](const char* marker) {
        const char* p = strstr(stage, marker);
        return p ? p + strlen(marker) : stage;
    };
    if (g_crashed) {
        CopyText(v.title, sizeof(v.title), "Something went wrong");
        CopyText(v.detail, sizeof(v.detail),
                 "The emulator crashed - crash.log is in /data/switch-emu/logs. Back to the launcher in a few seconds...");
        v.error = true;
    } else if (starts("Benchmark suite finished")) {
        CopyText(v.title, sizeof(v.title), "Benchmarks finished");
        CopyText(v.detail, sizeof(v.detail), "Real seconds per test - lower is faster.");
        v.progress = 1, v.finished = true;
    } else if (starts("Benchmark suite")) {
        CopyText(v.title, sizeof(v.title), "Running benchmarks");
        int run = 0, runs = 0;
        if (ParseFraction(stage, &run, &runs) >= 0) {
            v.progress = float(run - 1) / float(runs);
            snprintf(v.detail, sizeof(v.detail), "Run %d of %d%s%s", run, runs, strchr(stage, ':') ? ":" : "",
                     strchr(stage, ':') ? after(":") : "");
            if (strstr(stage, "press any button"))
                snprintf(v.detail, sizeof(v.detail), "Run %d of %d next - press any button to stop the benchmarks", run, runs);
        } else {
            const char* rest = after("suite");
            while (*rest == ':' || *rest == ' ')
                rest++;
            CopyText(v.detail, sizeof(v.detail), rest);
        }
    } else if (starts("Installing")) {
        CopyText(v.title, sizeof(v.title), "Installing updates and DLC");
        int file = 0, files = 0;
        const char* percent = strrchr(stage, ':');
        if (ParseFraction(stage, &file, &files) >= 0) {
            const float done = percent ? float(atoi(percent + 1)) / 100.0f : 0.0f;
            v.progress = (float(file - 1) + done) / float(files);
            char name[kLineLen];
            CopyText(name, sizeof(name), stage + strlen("Installing "));
            if (char* paren = strstr(name, " (")) // "<file> (i/n): p%"
                *paren = 0;
            snprintf(v.detail, sizeof(v.detail), "%s - file %d of %d%s%s", name, file, files, percent ? "," : "",
                     percent ? percent + 1 : "");
        } else {
            CopyText(v.detail, sizeof(v.detail), "Getting ready...");
        }
    } else if (starts("Installed") || strstr(stage, "back to the menu")) {
        CopyText(v.title, sizeof(v.title), strstr(stage, "fail") || strstr(stage, "Could not") ? "Couldn't finish" : "Done");
        CopyText(v.detail, sizeof(v.detail), stage);
        v.finished = true;
        v.error = strstr(stage, "fail") || strstr(stage, "Could not");
    } else if (starts("Reading game info")) {
        CopyText(v.title, sizeof(v.title), "Reading your games");
        int game = 0, games = 0;
        v.progress = ParseFraction(stage, &game, &games);
        snprintf(v.detail, sizeof(v.detail), "Names and icons: game %d of %d", game, games);
    } else if (starts("Running")) {
        CopyText(v.title, sizeof(v.title), "Running without graphics");
        CopyText(v.detail, sizeof(v.detail), stage + strlen("Running "));
        if (char* paren = strstr(v.detail, " (null renderer"))
            *paren = 0;
    } else if (starts("Loading failed") || starts("Could not load")) {
        CopyText(v.title, sizeof(v.title), "Couldn't start the game");
        CopyText(v.detail, sizeof(v.detail), stage);
        v.error = true;
    } else if (starts("Finished")) {
        CopyText(v.title, sizeof(v.title), "Finished");
        CopyText(v.detail, sizeof(v.detail), "The program exited.");
        v.finished = true;
    } else {
        CopyText(v.title, sizeof(v.title), stage[0] ? stage : "Starting");
    }
    return v;
}

// Eight dots around a circle, one bright, turning.
void DrawSpinner(int cx, int cy, double t) {
    const int lead = int(t * 8) % 8;
    for (int i = 0; i < 8; i++) {
        const float a = float(i) * 3.14159265f / 4;
        const int age = (lead - i + 8) % 8;
        Disc(cx + int(std::cos(a) * 14), cy + int(std::sin(a) * 14), 4, Mix(kAccent, Rgb(40, 52, 80), float(age) / 7));
    }
}

void DrawStatus(int tick) {
    (void)tick;
    char stage[kLineLen], path[256], heading[64];
    Lock();
    CopyText(stage, sizeof(stage), g_stage);
    CopyText(path, sizeof(path), g_watch_path);
    CopyText(heading, sizeof(heading), g_watch_heading);
    Unlock();
    const StatusView v = ReadStatus(stage);
    const double now = Elapsed();

    DrawBackground(v.error ? Rgb(150, 50, 60) : kAccent);
    DrawBrand();
    {
        const time_t clock_now = time(nullptr);
        if (const tm* local = localtime(&clock_now)) {
            char clock_text[16];
            snprintf(clock_text, sizeof(clock_text), "%d:%02d", local->tm_hour, local->tm_min);
            DrawText(kBold, kWidth - 80 - TextWidth(kBold, clock_text), 54, clock_text, kText);
        }
    }

    PollWatchedFile(path);
    const bool side = g_watch_count > 0 || heading[0];
    const int left_w = side ? 960 : kWidth - 200;

    // Title, detail, progress.
    constexpr int kX = 100, kTitleY = 170;
    int tx = kX;
    if (!v.finished && !v.error) {
        DrawSpinner(kX + 22, kTitleY + kTitleFontHeight / 2, now);
        tx += 64;
    }
    char title[96];
    CopyText(title, sizeof(title), v.title);
    Ellipsize(kTitleFont, title, sizeof(title), kX + left_w - tx);
    DrawText(kTitleFont, tx, kTitleY, title, v.error ? kBad : kWhite);
    int y = kTitleY + kTitleFontHeight + 16;
    if (v.detail[0]) {
        char detail[kLineLen];
        CopyText(detail, sizeof(detail), v.detail);
        Ellipsize(kBold, detail, sizeof(detail), left_w);
        DrawText(kBold, kX, y, detail, kText);
        y += kBoldFontHeight + 22;
    }
    if (v.progress >= 0) {
        RoundRect(kX, y, left_w, 12, 6, Rgb(40, 50, 76));
        const int fill = std::max(12, int(float(left_w) * std::clamp(v.progress, 0.0f, 1.0f)));
        RoundRect(kX, y, fill, 12, 6, v.error ? kBad : kAccent);
        y += 12 + 34;
    } else {
        y += 10;
    }

    // Activity: the latest trace lines.
    const int card_y = std::max(y, 400), card_h = 900 - card_y;
    Shadow(kX, card_y, left_w, card_h, 24);
    RoundRect(kX, card_y, left_w, card_h, 24, kCard);
    DrawText(kRegular, kX + 30, card_y + 22, "Activity", kAccent);
    {
        const int line_h = kStatusFontHeight + 4;
        const int top = card_y + 22 + line_h + 8, bottom = card_y + card_h - 20;
        const int visible = std::max(0, (bottom - top) / line_h);
        char lines[kLines][kLineLen];
        Lock();
        const int count = g_line_count < kLines ? g_line_count : kLines;
        const int first = count > visible ? count - visible : 0;
        for (int i = first; i < count; i++)
            memcpy(lines[i - first], g_lines[(g_line_count - count + i) % kLines], kLineLen);
        Unlock();
        for (int i = 0; i < count - first; i++) {
            Ellipsize(kRegular, lines[i], sizeof(lines[i]), left_w - 60);
            const bool newest = i == count - first - 1;
            DrawText(kRegular, kX + 30, top + i * line_h, lines[i], newest ? kText : kDim, kX + left_w - 30);
        }
    }

    // Results / output on the right.
    if (side) {
        const int px = kX + left_w + 60, pw = kWidth - px - 100, py = kTitleY, ph = 900 - kTitleY;
        Shadow(px, py, pw, ph, 24);
        RoundRect(px, py, pw, ph, 24, kCard);
        DrawText(kBold, px + 30, py + 26, heading[0] ? heading : "Output", kWhite, px + pw - 30);
        const int line_h = kStatusFontHeight + 6;
        int ly = py + 26 + kBoldFontHeight + 18;
        if (g_watch_count == 0)
            DrawText(kRegular, px + 30, ly, "Nothing yet", kDim);
        for (int i = 0; i < g_watch_count && i < kWatchLines && ly < py + ph - line_h; i++) {
            const char* text = g_watch_lines[i];
            const bool done = strncmp(text, "done", 4) == 0, total = strncmp(text, "end ", 4) == 0;
            const bool first_line = i == 0 && strstr(text, "lower is faster");
            if (first_line)
                continue; // the detail line says it
            char shown[kLineLen];
            CopyText(shown, sizeof(shown), text);
            // "name: values" too wide for one line: the name, then the values under it.
            char* split = strstr(shown, ": ");
            if (split && TextWidth(kRegular, shown) > pw - 60) {
                *split = 0;
                Ellipsize(kBold, shown, sizeof(shown), pw - 60);
                DrawText(kBold, px + 30, ly, shown, kWhite, px + pw - 30);
                ly += kBoldFontHeight + 2;
                char values[kLineLen];
                CopyText(values, sizeof(values), split + 2);
                Ellipsize(kRegular, values, sizeof(values), pw - 60);
                DrawText(kRegular, px + 30, ly, values, done ? kGood : kText, px + pw - 30);
                ly += line_h + 8;
                continue;
            }
            Ellipsize(kRegular, shown, sizeof(shown), pw - 60);
            DrawText(kRegular, px + 30, ly, shown, done ? kGood : total ? kWhite : kText, px + pw - 30);
            ly += line_h;
        }
    }

    // Hints.
    int hx = kX;
    const int hy = 936;
    if (g_crashed) {
        Hint(hx, hy, "Back to the launcher in a few seconds");
    } else if ((v.finished && strstr(stage, "Benchmark")) || strstr(stage, "Press any button for the launcher")) {
        hx = DrawButton(hx, hy, Button::Cross);
        Hint(hx, hy, "Back to the launcher (any button)");
    } else if (strstr(stage, "press any button")) {
        Hint(hx, hy, "Press any button to stop");
    } else if (strstr(stage, "Benchmark suite")) {
        hx = DrawButton(hx, hy, Button::Circle);
        Hint(hx, hy, "Cancel and go back to the launcher");
    } else if (strstr(stage, "Install")) {
        Hint(hx, hy, "Back to the launcher by itself when the install is done");
    } else {
        Hint(hx, hy, "Please wait - back to the launcher when it's done");
    }
    const char* logs = "Logs: /data/switch-emu/logs";
    DrawText(kRegular, kWidth - 100 - TextWidth(kRegular, logs), hy + 8, logs, kDim);
}

void DrawFrame(int tick) {
    if (g_launcher_on) {
        DrawLauncher();
        return;
    }
    // A game is starting with graphics on: the loading screen right away (the launch zoom ends on
    // it), until the Vulkan swap chain takes over the screen.
    struct stat st;
    if (g_loading_title[0] && !g_crashed && stat("/data/switch-emu/null-renderer", &st) != 0) {
        DrawLoading("Starting...");
        return;
    }
    DrawStatus(tick);
}

// --- VideoOut ----------------------------------------------------------------------------------

void* ThreadMain(void*) {
    const int32_t handle = sceVideoOutOpen(kUserIdSystem, kBusMain, 0, nullptr);
    if (handle < 0) {
        fprintf(stderr, "[status] sceVideoOutOpen failed: %#x\n", unsigned(handle));
        g_running = false;
        g_ready = true;
        return nullptr;
    }
    off_t phys = 0;
    void* mapped = nullptr;
    const size_t size = kFrameAligned * kBuffers;
    if (sceKernelAllocateDirectMemory(0, off_t(sceKernelGetDirectMemorySize()), size, 2u << 20, kWcGarlic, &phys) != 0 ||
        sceKernelMapDirectMemory(&mapped, size, kProtCpuGpuRw, 0, phys, 2u << 20) != 0) {
        fprintf(stderr, "[status] framebuffer allocation failed\n");
        sceVideoOutClose(handle);
        g_running = false;
        g_ready = true;
        return nullptr;
    }
    void* buffers[kBuffers];
    for (int i = 0; i < kBuffers; i++)
        buffers[i] = static_cast<char*>(mapped) + kFrameAligned * i;
    BufferAttribute attr{};
    attr.format = int32_t(0x80002200); // A8B8G8R8_SRGB
    attr.tmode = 1;                    // linear
    attr.aspect = 0;                   // 16:9
    attr.width = kWidth;
    attr.height = kHeight;
    attr.pixel_pitch = kWidth;
    const int32_t set = sceVideoOutRegisterBuffers(handle, 0, buffers, kBuffers, &attr);
    sceVideoOutSetFlipRate(handle, 0);
    g_ready = true;

    g_frame = static_cast<uint32_t*>(malloc(kFrameBytes));
    int current = 0;
    for (int tick = 0; !g_stop && g_frame; tick++) {
        DrawFrame(tick);
        while (sceVideoOutIsFlipPending(handle) > 0)
            usleep(1000);
        memcpy(buffers[current], g_frame, kFrameBytes);
        sceVideoOutSubmitFlip(handle, current, kFlipVsync, tick);
        current = (current + 1) % kBuffers;
        // A few frames a second, or at once when the launcher changed (button presses).
        if (g_launcher_on && g_animating) {
            usleep(16 * 1000); // something moves: ~60 Hz
            continue;
        }
        const int seen = g_dirty.load();
        for (int waited = 0; waited < 250 && g_dirty.load() == seen && !g_stop; waited += 8)
            usleep(8 * 1000);
    }

    while (sceVideoOutIsFlipPending(handle) > 0)
        usleep(1000);
    if (set >= 0)
        sceVideoOutUnregisterBuffers(handle, set);
    sceVideoOutClose(handle);
    sceKernelMunmap(mapped, size);
    sceKernelReleaseDirectMemory(phys, size);
    free(g_frame);
    g_frame = nullptr;
    return nullptr;
}

} // namespace

extern "C" {

void ps4_status_push(const char* line) {
    Lock();
    CopyText(g_lines[g_line_count % kLines], kLineLen, line);
    g_line_count++;
    Unlock();
}

void ps4_status_set_stage(const char* stage) {
    Lock();
    CopyText(g_stage, sizeof(g_stage), stage);
    Unlock();
}

// Shows the given file's lines on the right (re-read as it grows). Switching files starts over.
void ps4_status_watch_file(const char* path, const char* heading) {
    Lock();
    CopyText(g_watch_path, sizeof(g_watch_path), path);
    CopyText(g_watch_heading, sizeof(g_watch_heading), heading);
    g_watch_count = 0;
    g_begin_name[0] = 0;
    Unlock();
}

// Shows (count > 0) or hides (count == 0) the menu.
void ps4_status_set_menu(const char* const* lines, int count) {
    Lock();
    g_menu_count = count < kMenuLines ? count : kMenuLines;
    for (int i = 0; i < g_menu_count; i++)
        CopyText(g_menu[i], kLineLen, lines[i]);
    Unlock();
}

// Copies the watched file's lines as shown (with real-time annotations), newline separated.
int ps4_status_copy_watch(char* out, int size) {
    int used = 0;
    Lock();
    for (int i = 0; i < g_watch_count && i < kWatchLines && used + kLineLen + 2 < size; i++)
        used += snprintf(out + used, size - used, "%s\n", g_watch_lines[i]);
    Unlock();
    if (size > 0)
        out[used < size ? used : size - 1] = 0;
    return used;
}

void ps4_launcher_set_games(const Ps4LauncherGame* games, int count) {
    Lock();
    for (int i = 0; i < g_game_count; i++) { // scaled icons of the previous list
        free(g_games[i].small);
        free(g_games[i].big);
        g_games[i].small = g_games[i].big = nullptr;
        g_games[i].small_size = g_games[i].big_size = 0;
    }
    g_game_count = count < kMaxGames ? count : kMaxGames;
    for (int i = 0; i < g_game_count; i++) {
        CopyText(g_games[i].title, sizeof(g_games[i].title), games[i].title);
        CopyText(g_games[i].detail, sizeof(g_games[i].detail), games[i].detail);
        g_games[i].icon = games[i].icon;
        g_games[i].accent = games[i].icon ? AverageColor(games[i].icon) : TileColor(g_games[i].title);
    }
    Unlock();
    g_dirty++;
}

void ps4_launcher_set_settings(const Ps4LauncherSetting* settings, int count) {
    SettingEntry fresh[kMaxSettings]{};
    const int n = count < kMaxSettings ? count : kMaxSettings;
    for (int i = 0; i < n; i++) {
        CopyText(fresh[i].label, sizeof(fresh[i].label), settings[i].label);
        CopyText(fresh[i].value, sizeof(fresh[i].value), settings[i].value);
        CopyText(fresh[i].help, sizeof(fresh[i].help), settings[i].help);
        CopyText(fresh[i].group, sizeof(fresh[i].group), settings[i].group);
        fresh[i].kind = settings[i].kind;
    }
    Lock();
    // The menu sends these on every poll: only a change needs a redraw.
    const bool changed = n != g_setting_count || memcmp(fresh, g_settings, sizeof(SettingEntry) * size_t(n)) != 0;
    if (changed) {
        g_setting_count = n;
        memcpy(g_settings, fresh, sizeof(SettingEntry) * size_t(n));
    }
    Unlock();
    if (changed)
        g_dirty++;
}

void ps4_launcher_set_layout(int layout, int shortcut) {
    Lock();
    const bool changed = layout != g_layout || shortcut != g_shortcut;
    g_layout = layout;
    g_shortcut = shortcut;
    Unlock();
    if (changed)
        g_dirty++;
}

void ps4_launcher_set_state(int tab, int game, int setting) {
    Lock();
    const bool changed = tab != g_tab || game != g_game_sel || setting != g_setting_sel;
    g_tab = tab;
    g_game_sel = game;
    g_setting_sel = setting;
    Unlock();
    if (changed)
        g_dirty++;
}

void ps4_launcher_set_info(int install_count, bool has_pad) {
    Lock();
    const bool changed = install_count != g_install_count || has_pad != g_has_pad;
    g_install_count = install_count;
    g_has_pad = has_pad;
    Unlock();
    if (changed)
        g_dirty++;
}

void ps4_launcher_set_scope(const char* text, bool switchable) {
    char fresh[kLineLen];
    CopyText(fresh, sizeof(fresh), text);
    Lock();
    const bool changed = strcmp(fresh, g_scope) != 0 || switchable != g_scope_switchable;
    memcpy(g_scope, fresh, sizeof(g_scope));
    g_scope_switchable = switchable;
    Unlock();
    if (changed)
        g_dirty++;
}

void ps4_launcher_toast(const char* text) {
    Lock();
    CopyText(g_toast, sizeof(g_toast), text);
    g_toast_until = Elapsed() + 2.5;
    Unlock();
    g_dirty++;
}

void ps4_launcher_show(bool on) {
    g_launcher_on = on;
    g_launch_t0 = -1;
    g_dirty++;
}

void ps4_launcher_launch() {
    g_launch_snapped = false;
    g_launch_t0 = Elapsed();
    g_dirty++;
    usleep(340 * 1000); // the zoom (DrawLaunch) takes 0.32 s
}

const char* ps4_launcher_loading_title() {
    return g_loading_title;
}

void ps4_launcher_set_loading(const char* title, const uint32_t* icon) {
    CopyText(g_loading_title, sizeof(g_loading_title), title);
    g_loading_icon = icon;
    g_load.stage = 0;
    g_load.cache_done = g_load.cache_total = 0;
    g_load.shown = g_load.stage_time = NAN;
}

void ps4_loading_cache(unsigned done, unsigned total) {
    if (g_load.stage.exchange(1) != 1)
        g_load.stage_time = Elapsed();
    g_load.cache_done = done;
    g_load.cache_total = total;
}

void ps4_loading_starting_game() {
    g_load.shaders_at_start = ps4_pipelines_built.load();
    g_load.stage_time = Elapsed();
    g_load.stage = 2;
}

// Draws the loading screen into dst (1920x1080, pitch in pixels). Called from the Vulkan swap
// chain's thread while the status thread is stopped.
void ps4_status_render_loading(uint32_t* dst, int pitch, const char* progress) {
    static uint32_t* frame = nullptr;
    if (!frame)
        frame = static_cast<uint32_t*>(malloc(kFrameBytes));
    if (!frame || !dst)
        return;
    uint32_t* const saved = g_frame;
    g_frame = frame;
    DrawLoading(progress ? progress : "");
    g_frame = saved;
    for (int y = 0; y < kHeight; y++)
        memcpy(dst + size_t(y) * size_t(pitch), frame + size_t(y) * kWidth, size_t(kWidth) * 4);
}

// Home menu: keeps a darkened copy of src (the game's frame; read once - GPU memory is slow to
// read from the CPU) when capture is set, then draws the menu into dst.
void ps4_status_draw_home(uint32_t* dst, const uint32_t* src, int pitch, const Ps4HomeMenu* menu, bool capture) {
    static uint32_t* frame = nullptr;
    if (!frame)
        frame = static_cast<uint32_t*>(malloc(kFrameBytes));
    if (!g_pause_bg)
        g_pause_bg = static_cast<uint32_t*>(malloc(kFrameBytes));
    if (!frame || !dst || !menu)
        return;
    if (capture) { // the menu was just opened
        g_pause_anim.opened = Elapsed();
        g_pause_anim.last = g_pause_anim.opened;
        g_pause_anim.highlight = -1;
    }
    if (capture && src && g_pause_bg) {
        for (int y = 0; y < kHeight; y++) {
            const uint32_t* row = src + size_t(y) * size_t(pitch);
            uint32_t* out = g_pause_bg + size_t(y) * kWidth;
            memcpy(out, row, size_t(kWidth) * 4);
            for (int x = 0; x < kWidth; x++) // darken to ~35%
                out[x] = 0xFF000000u | ((out[x] >> 2) & 0x003F3F3Fu) | ((out[x] >> 3) & 0x001F1F1Fu);
        }
    }
    uint32_t* const saved = g_frame;
    g_frame = frame;
    DrawPause(*menu);
    g_frame = saved;
    for (int y = 0; y < kHeight; y++)
        memcpy(dst + size_t(y) * size_t(pitch), frame + size_t(y) * kWidth, size_t(kWidth) * 4);
}

// True while the Home menu is still animating (the caller keeps redrawing it).
bool ps4_status_home_animating() {
    return PauseAnimating();
}

// Performance overlay: rendered into a w x h panel (top-left of a scratch frame, then copied out).
void ps4_status_render_overlay(uint32_t* dst, int w, int h, const Ps4OverlayStats* st) {
    static uint32_t* frame = nullptr;
    if (!frame)
        frame = static_cast<uint32_t*>(malloc(kFrameBytes));
    if (!frame || !dst || !st || w > kWidth || h > kHeight)
        return;
    uint32_t* const saved = g_frame;
    g_frame = frame;
    FillRect(0, 0, w, h, Rgb(10, 13, 24));
    const uint32_t fps_color = st->fps >= 55 ? Rgb(110, 220, 140) : st->fps >= 30 ? Rgb(250, 205, 90) : kBad;
    FillRect(0, 0, 6, h, fps_color);
    char line[96];
    snprintf(line, sizeof(line), "%.0f", st->fps);
    if (st->mode == kOverlayFps) {
        const int x = 22;
        const int after = DrawText(kTitleFont, x, (h - kTitleFontHeight) / 2, line, fps_color);
        DrawText(kBold, after + 10, (h - kBoldFontHeight) / 2 + 6, "FPS", kDim);
    } else {
        int y = 8;
        const int after = DrawText(kTitleFont, 22, y, line, fps_color);
        DrawText(kBold, after + 10, y + 24, "FPS", kDim);
        y += kTitleFontHeight + 2;
        const auto row = [&](const char* label, const char* value, uint32_t color) {
            DrawText(kRegular, 22, y, label, kDim);
            DrawText(kBold, w - 20 - TextWidth(kBold, value), y - 2, value, color);
            y += kStatusFontHeight + 3;
        };
        snprintf(line, sizeof(line), "%.1f ms", st->frame_ms);
        row("Frame time", line, kText);
        snprintf(line, sizeof(line), "%.1f ms", st->worst_ms);
        row("Worst frame", line, st->worst_ms > 50 ? kBad : kText);
        snprintf(line, sizeof(line), "%.0f%%", st->gpu_wait * 100);
        row("Waiting for GPU", line, st->gpu_wait > 0.3f ? Rgb(250, 205, 90) : kText);
        snprintf(line, sizeof(line), "%d", st->new_shaders);
        row("New shaders", line, kText);
        snprintf(line, sizeof(line), "%d MB", st->gpu_free_mb);
        row("GPU memory free", line, st->gpu_free_mb < 128 ? kBad : kText);
    }
    g_frame = saved;
    for (int y = 0; y < h; y++)
        memcpy(dst + size_t(y) * size_t(w), frame + size_t(y) * kWidth, size_t(w) * 4);
}

void ps4_status_crashed() {
    g_crashed = true;
}

void ps4_status_start(const char* title) {
    if (g_running.exchange(true))
        return;
    Lock();
    CopyText(g_title, sizeof(g_title), title);
    Unlock();
    clock_gettime(CLOCK_MONOTONIC, &g_start);
    g_stop = false;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    g_ready = false;
    if (pthread_create(&g_thread, &attr, ThreadMain, nullptr) != 0) {
        g_running = false;
        g_ready = true;
    }
    pthread_attr_destroy(&attr);
    // Hold VideoOut before anything else (SDL) can open it.
    for (int i = 0; i < 2000 && !g_ready; i++)
        usleep(1000);
}

// Closes VideoOut so a renderer can open it.
void ps4_status_stop() {
    if (!g_running)
        return;
    g_stop = true;
    pthread_join(g_thread, nullptr);
    g_running = false;
}

} // extern "C"
