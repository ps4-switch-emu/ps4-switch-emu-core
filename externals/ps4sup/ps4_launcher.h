// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The start screen ("launcher"): a game library and a settings tab, drawn by status_screen.cpp
// and driven by ps4_menu.cpp. The menu owns the data it passes in (icons stay valid while shown).

#pragma once

#include <cstdint>

constexpr int kLauncherIconSize = 256;
constexpr int kLauncherColumns = 8; // games grid: Up/Down move by a row // icons: kLauncherIconSize^2 pixels, framebuffer byte order (RGBA)

struct Ps4LauncherGame {
    const char* title;
    const char* detail;   // second line under the title (file, size, ...)
    const uint32_t* icon; // nullptr: a coloured tile with the title's initials
};

// How a settings row's value is shown.
enum Ps4SettingKind { kSettingText = 0, kSettingToggle = 1, kSettingChoice = 2, kSettingAction = 3 };

struct Ps4LauncherSetting {
    const char* label;
    const char* value;
    const char* help;            // shown beside the list for the selected row
    const char* group = nullptr; // section heading, drawn where it changes
    int kind = kSettingText;     // toggle: value "On"/"Off"; choice: drawn as < value >
};

struct Ps4HomeMenu {
    const char* title;  // the game's
    const char* stats;  // e.g. the frame rate
    const char* const* items;
    int count;
    int selected;
};

// In-game performance overlay (vk_present_manager.cpp copies it into each frame).
enum Ps4OverlayMode { kOverlayOff = 0, kOverlayFps = 1, kOverlayDetailed = 2 };
enum Ps4OverlayCorner { kOverlayTopLeft = 0, kOverlayTopRight = 1, kOverlayBottomLeft = 2, kOverlayBottomRight = 3 };
struct Ps4OverlayStats {
    int mode;
    float fps;         // presented frames per second (last half second)
    float frame_ms;    // average time between frames
    float worst_ms;    // longest gap in the window
    float gpu_wait;    // share of time waiting for the GPU, 0..1
    int new_shaders;   // pipelines built in game so far
    int gpu_free_mb;   // largest free block of direct memory
};
constexpr int kOverlayFpsW = 190, kOverlayFpsH = 70, kOverlayDetailW = 430, kOverlayDetailH = 292;

enum Ps4LauncherTab { kLauncherGames = 0, kLauncherSettings = 1 };

// Games tab: a grid, or one row of large tiles with round shortcuts below (console style).
enum Ps4LauncherLayout { kLayoutGrid = 0, kLayoutRow = 1 };
// The row layout's shortcuts, left to right.
enum Ps4LauncherShortcut { kShortcutSettings = 0, kShortcutInstall = 1, kShortcutBackup = 2, kShortcutCount = 3 };

extern "C" {
void ps4_launcher_set_games(const Ps4LauncherGame* games, int count);
void ps4_launcher_set_settings(const Ps4LauncherSetting* settings, int count);
void ps4_launcher_set_state(int tab, int game, int setting);
// The games tab's layout and, in the row layout, the selected shortcut (-1: the games row).
void ps4_launcher_set_layout(int layout, int shortcut);
// The button hints' install count, and whether a controller is connected (else a warning).
void ps4_launcher_set_info(int install_count, bool has_pad);
void ps4_launcher_show(bool on);
// The chosen game's card zooms into the loading screen (blocks for the animation, ~1/3 s).
void ps4_launcher_launch();
// A short message shown at the bottom for a few seconds (e.g. "Backup complete").
void ps4_launcher_toast(const char* text);
// Settings tab heading ("All games" or a game's title) and whether Square switches it.
void ps4_launcher_set_scope(const char* text, bool switchable);
// The game being started, for the loading screen (icon may be nullptr; must stay valid).
void ps4_launcher_set_loading(const char* title, const uint32_t* icon);
// Loading screen stages: shaders loaded from the cache so far, then the game starting (until its
// first frame - the swap chain redraws the screen meanwhile, see vk_swapchain.cpp).
void ps4_loading_cache(unsigned done, unsigned total);
void ps4_loading_starting_game();
// In-game Home menu into a swap image (see vk_swapchain.cpp's overlay); capture: take src as the
// new background.
void ps4_status_draw_home(uint32_t* dst, const uint32_t* src, int pitch, const Ps4HomeMenu* menu, bool capture);
// True while the Home menu animates (opening, the highlight moving): keep redrawing it.
bool ps4_status_home_animating();
// Draws the overlay panel (kOverlayFpsW x kOverlayFpsH or the detailed size) into dst.
void ps4_status_render_overlay(uint32_t* dst, int w, int h, const Ps4OverlayStats* stats);
// The title of the game being played (for the Home menu).
const char* ps4_launcher_loading_title();
}
