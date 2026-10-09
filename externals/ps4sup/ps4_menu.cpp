// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// PS4 start menu and benchmark suite (status screen + DualShock 4).
//
// The menu sets Eden's PS4 test switches - files in /data/switch-emu/ that eden-cli and HostMemory
// already read (singlecore, no-fastmem, cpu-unsafe, null-renderer) - so the choices persist and
// nothing else needs to know about the menu. "Run all benchmarks" runs test.nro once per
// configuration: settings like multi-core and fastmem only apply to a fresh process, so the app
// restarts itself between runs (sceSystemServiceLoadExec, as DolphinPS4 does) and keeps its place
// in /data/switch-emu/suite.txt. Each run adds one summary line to /data/switch-emu/logs/bench-results.txt.

#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <thread>
#include <string>
#include <vector>

#include "ps4_launcher.h"

// stb_image, compiled into Eden (src/common/stb.cpp).
extern "C" unsigned char* stbi_load(const char* filename, int* x, int* y, int* comp, int req_comp);
extern "C" void stbi_image_free(void* retval_from_stbi_load);
extern "C" unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len, int* x, int* y, int* comp,
                                                int req_comp);

extern "C" {
void ps4_status_set_menu(const char* const* lines, int count);
void ps4_status_set_stage(const char* stage);
void ps4_status_watch_file(const char* path, const char* heading);
int ps4_status_copy_watch(char* out, int size);
void ps4_boot_trace(const char* stage);
int32_t sceUserServiceGetInitialUser(int32_t* user);
int32_t scePadInit();
int32_t scePadOpen(int32_t user, int32_t type, int32_t index, const void* param);
int32_t scePadClose(int32_t handle);
int32_t scePadReadState(int32_t handle, void* data);
int sceSystemServiceLoadExec(const char* path, char* const argv[]);
int32_t sceAudioOutInit();
int32_t sceAudioOutOpen(int32_t user, int32_t port_type, int32_t index, uint32_t length, uint32_t rate, uint32_t format);
int32_t sceAudioOutOutput(int32_t handle, const void* data);
int32_t sceAudioOutClose(int32_t handle);
}

namespace {

constexpr const char* kDir = "/data/switch-emu/";
constexpr const char* kSuiteState = "/data/switch-emu/suite.txt";
// Left by a program that exited on its own (cleared when the menu shows; kept for older notes).
constexpr const char* kBackToMenu = "/data/switch-emu/back-to-menu";
constexpr const char* kResults = "/data/switch-emu/logs/bench-results.txt";
constexpr const char* kGamesDir = "/data/switch-emu/games/";
constexpr const char* kGameChoice = "/data/switch-emu/game.txt";
constexpr const char* kBenchmark = "test.nro";
std::string g_game_path = std::string(kGamesDir) + kBenchmark;

// Files in /data/switch-emu/games that Eden can load, sorted.
std::vector<std::string> ListGames() {
    std::vector<std::string> games;
    if (DIR* dir = opendir(kGamesDir)) {
        while (dirent* entry = readdir(dir)) {
            const std::string name = entry->d_name;
            const auto dot = name.rfind('.');
            if (name[0] == '.' || dot == std::string::npos)
                continue;
            std::string ext = name.substr(dot + 1);
            for (char& c : ext)
                c = char(tolower(c));
            if (ext == "nro" || ext == "nso" || ext == "nsp" || ext == "xci" || ext == "nca")
                games.push_back(name);
        }
        closedir(dir);
    }
    std::sort(games.begin(), games.end());
    return games;
}

// Update/DLC .nsp files waiting in /data/switch-emu/install (installed by a "install-now" run of Eden).
int CountInstallFiles() {
    int count = 0;
    if (DIR* dir = opendir("/data/switch-emu/install")) {
        while (dirent* entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name.size() > 4 && strcasecmp(name.c_str() + name.size() - 4, ".nsp") == 0)
                count++;
        }
        closedir(dir);
    }
    return count;
}

#ifdef __OPENORBIS__
struct SceKernelSwVersion {
    size_t size;
    char version_string[0x1C];
    uint32_t version; // e.g. 0x13020000 for 13.02
};
extern "C" int sceKernelGetSystemSwVersion(SceKernelSwVersion* out);
#endif

// Settings > About: this package's version (written into it by scripts/package-eden.sh).
std::string AppVersion() {
    char text[32] = "";
    if (FILE* f = fopen("/app0/version.txt", "r")) {
        if (!fgets(text, sizeof(text), f))
            text[0] = 0;
        fclose(f);
    }
    text[strcspn(text, "\r\n")] = 0;
    return text[0] ? std::string("v") + text : "Unknown";
}

// Settings > About: the console's system software version.
std::string Ps4Firmware() {
#ifdef __OPENORBIS__
    SceKernelSwVersion v{};
    v.size = sizeof(v);
    if (sceKernelGetSystemSwVersion(&v) == 0 && v.version != 0) {
        char text[16];
        snprintf(text, sizeof(text), "%x.%02x", v.version >> 24, (v.version >> 16) & 0xFF);
        return text;
    }
#endif
    return "Unknown";
}

// What commercial games need from the user's own Switch, for Settings > Setup.
struct Setup {
    bool prod_keys = false, title_keys = false;
    int firmware_files = 0;
    bool Ready() const { return prod_keys && firmware_files > 0; }
};

Setup CheckSetup() {
    Setup s;
    struct stat st;
    s.prod_keys = stat("/data/switch-emu/keys/prod.keys", &st) == 0;
    s.title_keys = stat("/data/switch-emu/keys/title.keys", &st) == 0;
    int ncas = 0;
    if (DIR* dir = opendir("/data/switch-emu/nand/system/Contents/registered")) {
        while (dirent* entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name.size() > 4 && strcasecmp(name.c_str() + name.size() - 4, ".nca") == 0)
                ncas++;
        }
        closedir(dir);
    }
    s.firmware_files = ncas;
    return s;
}

// Library entries: the real title once the game has run (Eden saves it, with the icon, to
// /data/switch-emu/titles/<file>.txt/.jpg), otherwise the file name without its [ids] and (tags).
constexpr const char* kTitlesDir = "/data/switch-emu/titles/";

std::string CleanName(const std::string& file) {
    std::string out;
    int depth = 0;
    const size_t dot = file.rfind('.');
    for (size_t i = 0; i < (dot == std::string::npos ? file.size() : dot); i++) {
        const char c = file[i];
        if (c == '[' || c == '(')
            depth++;
        else if ((c == ']' || c == ')') && depth > 0)
            depth--;
        else if (depth == 0)
            out += (c == '_') ? ' ' : c;
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    while (!out.empty() && out.front() == ' ')
        out.erase(out.begin());
    return out.empty() ? file : out;
}

// Homebrew (.nro) carries its title and icon unencrypted: an "ASET" block after the NRO image
// (icon JPEG, NACP). Read from there when Eden hasn't cached anything yet.
struct NroAssets {
    std::string title;
    std::vector<uint8_t> icon_jpeg;
};

bool ReadNroAssets(const std::string& file, NroAssets& out) {
    FILE* f = fopen((std::string(kGamesDir) + file).c_str(), "rb");
    if (!f)
        return false;
    bool ok = false;
    uint8_t header[0x80];
    if (fread(header, 1, sizeof(header), f) == sizeof(header) && memcmp(header + 0x10, "NRO0", 4) == 0) {
        uint32_t nro_size;
        memcpy(&nro_size, header + 0x18, 4);
        uint8_t aset[0x38];
        if (fseek(f, long(nro_size), SEEK_SET) == 0 && fread(aset, 1, sizeof(aset), f) == sizeof(aset) &&
            memcmp(aset, "ASET", 4) == 0) {
            uint64_t icon_off, icon_size, nacp_off, nacp_size;
            memcpy(&icon_off, aset + 0x08, 8);
            memcpy(&icon_size, aset + 0x10, 8);
            memcpy(&nacp_off, aset + 0x18, 8);
            memcpy(&nacp_size, aset + 0x20, 8);
            if (nacp_size >= 0x200 && fseek(f, long(nro_size + nacp_off), SEEK_SET) == 0) {
                char name[0x201] = {};
                if (fread(name, 1, 0x200, f) == 0x200)
                    out.title = name; // first entry: American English
            }
            if (icon_size > 0 && icon_size < (4u << 20) && fseek(f, long(nro_size + icon_off), SEEK_SET) == 0) {
                out.icon_jpeg.resize(size_t(icon_size));
                if (fread(out.icon_jpeg.data(), 1, out.icon_jpeg.size(), f) != out.icon_jpeg.size())
                    out.icon_jpeg.clear();
            }
            ok = true;
        }
    }
    fclose(f);
    return ok;
}

bool IsNro(const std::string& file) {
    return file.size() > 4 && strcasecmp(file.c_str() + file.size() - 4, ".nro") == 0;
}

std::string CachedTitle(const std::string& file) {
    char line[256] = {};
    if (FILE* f = fopen((std::string(kTitlesDir) + file + ".txt").c_str(), "r")) {
        if (!fgets(line, sizeof(line), f))
            line[0] = 0;
        fclose(f);
    }
    std::string title = line;
    while (!title.empty() && (title.back() == '\n' || title.back() == '\r'))
        title.pop_back();
    if (title.empty() && IsNro(file)) {
        NroAssets assets;
        if (ReadNroAssets(file, assets))
            title = assets.title;
    }
    return title.empty() ? CleanName(file) : title;
}

// The cached icon scaled to kLauncherIconSize, or empty.
std::vector<uint32_t> CachedIcon(const std::string& file) {
    int w = 0, h = 0, channels = 0;
    unsigned char* rgba = stbi_load((std::string(kTitlesDir) + file + ".jpg").c_str(), &w, &h, &channels, 4);
    if (!rgba && IsNro(file)) {
        NroAssets assets;
        if (ReadNroAssets(file, assets) && !assets.icon_jpeg.empty())
            rgba = stbi_load_from_memory(assets.icon_jpeg.data(), int(assets.icon_jpeg.size()), &w, &h, &channels, 4);
    }
    if (!rgba || w <= 0 || h <= 0) {
        if (rgba)
            stbi_image_free(rgba);
        return {};
    }
    std::vector<uint32_t> icon(size_t(kLauncherIconSize) * kLauncherIconSize);
    const auto* src = reinterpret_cast<const uint32_t*>(rgba);
    for (int y = 0; y < kLauncherIconSize; y++)
        for (int x = 0; x < kLauncherIconSize; x++)
            icon[size_t(y) * kLauncherIconSize + x] = src[size_t(y * h / kLauncherIconSize) * w + x * w / kLauncherIconSize];
    stbi_image_free(rgba);
    return icon;
}

// When each game was last started (/data/switch-emu/titles/<file>.last), for "recently played first".
long LastPlayed(const std::string& file) {
    long t = 0;
    if (FILE* f = fopen((std::string(kTitlesDir) + file + ".last").c_str(), "r")) {
        if (fscanf(f, "%ld", &t) != 1)
            t = 0;
        fclose(f);
    }
    return t;
}

void MarkPlayed(const std::string& file) {
    mkdir(kTitlesDir, 0777);
    if (FILE* f = fopen((std::string(kTitlesDir) + file + ".last").c_str(), "w")) {
        fprintf(f, "%ld\n", long(time(nullptr)));
        fclose(f);
    }
}

// Play time across sessions (/data/switch-emu/titles/<file>.play: "<seconds> <sessions>", kept by
// the emulator while a game runs).
long PlaySeconds(const std::string& file) {
    long seconds = 0, sessions = 0;
    if (FILE* f = fopen((std::string(kTitlesDir) + file + ".play").c_str(), "r")) {
        if (fscanf(f, "%ld %ld", &seconds, &sessions) != 2)
            seconds = 0;
        fclose(f);
    }
    return seconds;
}

// As "  -  played 12 h 31 min" (nothing under a minute).
std::string PlayedDetail(const std::string& file) {
    const long seconds = PlaySeconds(file);
    char text[48];
    if (seconds >= 3600)
        snprintf(text, sizeof(text), "  -  played %ld h %ld min", seconds / 3600, seconds / 60 % 60);
    else if (seconds >= 60)
        snprintf(text, sizeof(text), "  -  played %ld min", seconds / 60);
    else
        text[0] = 0;
    return text;
}

// "today", "yesterday" or "Oct 08", for when a game was last played.
std::string PlayedWhen(long played) {
    const time_t then = time_t(played), now = time(nullptr);
    tm a{}, b{};
    if (!localtime_r(&then, &a) || !localtime_r(&now, &b))
        return "";
    a.tm_hour = b.tm_hour = 12, a.tm_min = b.tm_min = a.tm_sec = b.tm_sec = 0; // whole days apart
    const long days = long(difftime(mktime(&b), mktime(&a)) / 86400.0 + 0.5);
    if (days == 0)
        return "today";
    if (days == 1)
        return "yesterday";
    char when[32];
    localtime_r(&then, &a);
    strftime(when, sizeof(when), "%b %d", &a);
    return when;
}

std::string FileDetail(const std::string& file) {
    struct stat st;
    std::string ext = file.substr(file.rfind('.') + 1);
    for (char& c : ext)
        c = char(toupper(c));
    const char* kind = ext == "NRO" ? "Homebrew" : ext == "XCI" ? "Game card dump" : ext == "NSP" ? "Game" : "File";
    char text[160];
    if (stat((std::string(kGamesDir) + file).c_str(), &st) == 0) {
        const double mb = double(st.st_size) / (1024.0 * 1024.0);
        if (mb >= 1024)
            snprintf(text, sizeof(text), "%s  -  %s, %.1f GB", kind, ext.c_str(), mb / 1024.0);
        else
            snprintf(text, sizeof(text), "%s  -  %s, %.0f MB", kind, ext.c_str(), mb);
    } else {
        snprintf(text, sizeof(text), "%s  -  %s", kind, ext.c_str());
    }
    return text;
}

// Settings -> Back up saves: copies the emulated NAND's save data to a dated folder.
constexpr const char* kSaveRoot = "/data/switch-emu/nand/user/save";

bool CopyTree(const std::string& from, const std::string& to, int& files, long long& bytes) {
    DIR* dir = opendir(from.c_str());
    if (!dir)
        return false;
    mkdir(to.c_str(), 0777);
    bool ok = true;
    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..")
            continue;
        const std::string src = from + "/" + name, dst = to + "/" + name;
        struct stat st;
        if (stat(src.c_str(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            ok &= CopyTree(src, dst, files, bytes);
            continue;
        }
        FILE* in = fopen(src.c_str(), "rb");
        FILE* out = in ? fopen(dst.c_str(), "wb") : nullptr;
        if (!in || !out) {
            ok = false;
        } else {
            static char buffer[1 << 16];
            size_t n;
            while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
                if (fwrite(buffer, 1, n, out) != n) {
                    ok = false;
                    break;
                }
                bytes += long(n);
            }
            files++;
        }
        if (in)
            fclose(in);
        if (out)
            fclose(out);
    }
    closedir(dir);
    return ok;
}

// Returns a line for the settings row.
std::string BackUpSaves() {
    struct stat st;
    if (stat(kSaveRoot, &st) != 0)
        return "No saves yet";
    char name[96];
    const time_t now = time(nullptr);
    const tm* local = localtime(&now);
    if (local)
        strftime(name, sizeof(name), "/data/switch-emu/save-backups/%Y-%m-%d_%H-%M-%S", local);
    else
        snprintf(name, sizeof(name), "/data/switch-emu/save-backups/%ld", long(now));
    mkdir("/data/switch-emu/save-backups", 0777);
    int files = 0;
    long long bytes = 0;
    const bool ok = CopyTree(kSaveRoot, name, files, bytes);
    char line[160];
    snprintf(line, sizeof(line), "save backup: %s, %d files, %lld KiB -> %s", ok ? "done" : "INCOMPLETE", files,
             bytes / 1024, name);
    ps4_boot_trace(line);
    snprintf(line, sizeof(line), ok ? "Done: %d files" : "Failed (%d files)", files);
    return line;
}

std::string ReadGameChoice() {
    char line[512] = {};
    if (FILE* f = fopen(kGameChoice, "r")) {
        if (!fgets(line, sizeof(line), f))
            line[0] = 0;
        fclose(f);
    }
    std::string name = line;
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r'))
        name.pop_back();
    return name;
}

void WriteGameChoice(const std::string& name) {
    if (FILE* f = fopen(kGameChoice, "w")) {
        fprintf(f, "%s\n", name.c_str());
        fclose(f);
    }
}
constexpr uint32_t kUp = 0x0010, kDown = 0x0040, kLeft = 0x0080, kRight = 0x0020, kCross = 0x4000,
                   kOptions = 0x0008, kL1 = 0x0400, kR1 = 0x0800, kTriangle = 0x1000, kCircle = 0x2000,
                   kSquare = 0x8000;

struct PadData { // OrbisPadData: the button word and the left stick are used
    uint32_t buttons;
    uint8_t left_x, left_y; // 0..255, 128 centre (up is 0)
    uint8_t rest[0x200];    // larger than the real struct, whatever its size in this SDK version
};

bool FileExists(const char* name) {
    char path[128];
    snprintf(path, sizeof(path), "%s%s", kDir, name);
    struct stat st;
    return stat(path, &st) == 0;
}

void SetFile(const char* name, bool present) {
    char path[128];
    snprintf(path, sizeof(path), "%s%s", kDir, name);
    if (present) {
        if (FILE* f = fopen(path, "a"))
            fclose(f);
    } else {
        remove(path);
    }
}

struct Options {
    bool multicore, fastmem, cpu_unsafe, graphics;
    bool profile = false; ///< sampling profiler (externals/ps4sup/sampler.cpp)
    bool async_shaders = false;
    bool handheld = false; ///< console mode: games render at 720p instead of 1080p
    int resolution = 0;    ///< 0: 1x, 1: 0.75x, 2: 0.5x
    bool gpu_high = false; ///< GPU accuracy High (default Low)
    bool cross_is_a = false; ///< button layout: Cross = Switch A (else by position: Circle = A)
    bool spread_threads = false; ///< default-core guest threads may run on any of the game's cores
    int overlay = 0;        ///< performance overlay: Ps4OverlayMode
    int overlay_corner = 1; ///< Ps4OverlayCorner (top right)
    bool pin_threads = false; ///< busiest threads each on a PS4 core of their own (common/thread.cpp)
};

// /data/switch-emu/launcher.txt: the games tab's layout (Ps4LauncherLayout) and order, for all games.
constexpr const char* kLauncherFile = "/data/switch-emu/launcher.txt";
enum GameSort { kSortRecent = 0, kSortMostPlayed = 1, kSortName = 2, kSortCount = 3 };
const char* const kSortNames[kSortCount] = {"Recently played", "Most played", "A to Z"};

struct LauncherPrefs {
    int layout = kLayoutGrid;
    int sort = kSortRecent;
};

LauncherPrefs ReadLauncherPrefs() {
    LauncherPrefs p;
    if (FILE* f = fopen(kLauncherFile, "r")) {
        char key[32];
        int value = 0;
        while (fscanf(f, "%31s %d", key, &value) == 2) {
            if (strcmp(key, "layout") == 0)
                p.layout = std::clamp(value, 0, kLayoutCount - 1);
            else if (strcmp(key, "sort") == 0)
                p.sort = std::clamp(value, 0, kSortCount - 1);
        }
        fclose(f);
    }
    return p;
}

void SaveLauncherPrefs(const LauncherPrefs& p) {
    if (FILE* f = fopen(kLauncherFile, "w")) {
        fprintf(f, "layout %d\nsort %d\n", p.layout, p.sort);
        fclose(f);
    }
}

// /data/switch-emu/overlay.txt: "<mode> <corner>" for yuzu_cmd's performance overlay.
int ReadOverlay(int field) {
    int mode = 0, corner = 1;
    if (FILE* f = fopen("/data/switch-emu/overlay.txt", "r")) {
        if (fscanf(f, "%d %d", &mode, &corner) != 2)
            mode = 0, corner = 1;
        fclose(f);
    }
    return field == 0 ? std::clamp(mode, 0, 2) : std::clamp(corner, 0, 3);
}

Options ReadOptions() {
    return {!FileExists("singlecore"), !FileExists("no-fastmem"), FileExists("cpu-unsafe"),
            !FileExists("null-renderer"), FileExists("profile"), FileExists("async-shaders"),
            FileExists("handheld"), FileExists("res-050") ? 2 : FileExists("res-075") ? 1 : 0,
            FileExists("gpu-high"), FileExists("cross-is-a"), FileExists("spread-threads"), ReadOverlay(0),
            ReadOverlay(1), FileExists("pin-threads")};
}

void WriteOptions(const Options& o) {
    SetFile("singlecore", !o.multicore);
    SetFile("no-fastmem", !o.fastmem);
    SetFile("cpu-unsafe", o.cpu_unsafe);
    SetFile("null-renderer", !o.graphics);
    SetFile("profile", o.profile);
    SetFile("async-shaders", o.async_shaders);
    SetFile("handheld", o.handheld);
    SetFile("res-075", o.resolution == 1);
    SetFile("res-050", o.resolution == 2);
    SetFile("gpu-high", o.gpu_high);
    SetFile("cross-is-a", o.cross_is_a);
    SetFile("spread-threads", o.spread_threads);
    SetFile("pin-threads", o.pin_threads);
    if (FILE* f = fopen("/data/switch-emu/overlay.txt", "w")) {
        fprintf(f, "%d %d\n", o.overlay, o.overlay_corner);
        fclose(f);
    }
}

// Settings for all games live in /data/switch-emu/options.txt; a game's own in
// /data/switch-emu/titles/<file>.opts. The switch files above are what the next run uses (written at
// start from the game's settings, or the global ones).
constexpr const char* kGlobalOptions = "/data/switch-emu/options.txt";

bool LoadOptionsFile(const std::string& path, Options& o) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f)
        return false;
    char key[64];
    int value = 0;
    while (fscanf(f, " %63[^=]=%d", key, &value) == 2) {
        const std::string k = key;
        if (k == "multicore") o.multicore = value;
        else if (k == "fastmem") o.fastmem = value;
        else if (k == "cpu_unsafe") o.cpu_unsafe = value;
        else if (k == "graphics") o.graphics = value;
        else if (k == "profile") o.profile = value;
        else if (k == "async_shaders") o.async_shaders = value;
        else if (k == "handheld") o.handheld = value;
        else if (k == "resolution") o.resolution = value >= 0 && value <= 2 ? value : 0;
        else if (k == "gpu_high") o.gpu_high = value;
        else if (k == "cross_is_a") o.cross_is_a = value;
        else if (k == "spread_threads") o.spread_threads = value;
        else if (k == "pin_threads") o.pin_threads = value;
        else if (k == "overlay") o.overlay = std::clamp(value, 0, 2);
        else if (k == "overlay_corner") o.overlay_corner = std::clamp(value, 0, 3);
    }
    fclose(f);
    return true;
}

void SaveOptionsFile(const std::string& path, const Options& o) {
    if (FILE* f = fopen(path.c_str(), "w")) {
        fprintf(f, "multicore=%d\nfastmem=%d\ncpu_unsafe=%d\ngraphics=%d\nprofile=%d\nasync_shaders=%d\nhandheld=%d\n"
                   "resolution=%d\ngpu_high=%d\ncross_is_a=%d\nspread_threads=%d\noverlay=%d\noverlay_corner=%d\n"
                   "pin_threads=%d\n",
                o.multicore, o.fastmem, o.cpu_unsafe, o.graphics, o.profile, o.async_shaders, o.handheld, o.resolution,
                o.gpu_high, o.cross_is_a, o.spread_threads, o.overlay, o.overlay_corner, o.pin_threads);
        fclose(f);
    }
}

Options LoadGlobalOptions() {
    Options o = ReadOptions(); // before options.txt existed: the switch files were the settings
    // Save them as the all-games settings right away: from now on the switch files hold the last
    // run's settings, which may be one game's own.
    if (!LoadOptionsFile(kGlobalOptions, o))
        SaveOptionsFile(kGlobalOptions, o);
    return o;
}

struct SuiteRun {
    const char* label;
    Options options;
};
// All without graphics (the benchmark doesn't draw).
constexpr SuiteRun kSuite[] = {
    {"single-core, no fastmem", {false, false, false, false}},
    {"multi-core, no fastmem", {true, false, false, false}},
    {"single-core, fastmem", {false, true, false, false}},
    {"multi-core, fastmem", {true, true, false, false}},
    {"multi-core, fastmem, cpu-unsafe", {true, true, true, false}},
};
constexpr int kSuiteCount = int(sizeof(kSuite) / sizeof(kSuite[0]));

// The run in progress, or -1. Set by ps4_menu_run, used when the guest exits.
int g_suite_index = -1;
volatile bool g_restart_requested = false;
// A finished benchmark suite's results are up: any button goes back to the launcher.
std::atomic<bool> g_wait_for_button{false};
// ps4_menu_run returned: a program is starting (a crash from here on can go back to the launcher;
// one before it, in the launcher itself, must not restart into the same crash forever).
std::atomic<bool> g_program_started{false};

// suite.txt: "<index> <running>". running = 1 while that run is in progress: finding it set at
// startup means the run crashed (the app was restarted by hand).
int ReadSuiteIndex(bool* running) {
    *running = false;
    FILE* f = fopen(kSuiteState, "r");
    if (!f)
        return -1;
    int index = -1, state = 0;
    if (fscanf(f, "%d %d", &index, &state) < 1)
        index = -1;
    fclose(f);
    *running = state == 1;
    return index >= 0 && index < kSuiteCount ? index : -1;
}

void WriteSuiteIndex(int index, bool running = false) {
    if (index < 0) {
        remove(kSuiteState);
        return;
    }
    if (FILE* f = fopen(kSuiteState, "w")) {
        fprintf(f, "%d %d\n", index, running ? 1 : 0);
        fclose(f);
    }
}

void AppendResult(int index, const char* text) {
    if (FILE* f = fopen(kResults, "a")) {
        if (index == 0)
            fprintf(f, "Benchmark suite (real seconds, lower is faster)\n");
        fprintf(f, "%s: %s\n", kSuite[index].label, text);
        fclose(f);
    }
}

// Shows the results until a button is pressed, then goes back to the launcher (below Pad).
[[noreturn]] void ShowResultsForever();

// "end integer: ...  -> 6.0 s real" -> "6.0 s"
void FindResult(const char* text, const char* test, char* out, size_t size) {
    snprintf(out, size, "-");
    char key[32];
    snprintf(key, sizeof(key), "end %s", test);
    const char* line = strstr(text, key);
    if (!line)
        return;
    const char* arrow = strstr(line, "-> ");
    const char* eol = strchr(line, '\n');
    if (!arrow || (eol && arrow > eol))
        return;
    float seconds = 0;
    if (sscanf(arrow + 3, "%f", &seconds) == 1)
        snprintf(out, size, "%.1f s", seconds);
}

class Pad {
public:
    Pad() {
        scePadInit();
        Open();
    }
    ~Pad() {
        // SDL opens the pad again for the emulator.
        if (handle >= 0)
            scePadClose(handle);
    }
    // Newly pressed buttons since the last call; held D-pad directions repeat (scrolling).
    uint32_t Pressed() {
        if (handle < 0) {
            if (++retry % 60 == 0) // a controller turned on after Eden started
                Open();
            return 0;
        }
        PadData data{};
        data.left_x = data.left_y = 128;
        if (scePadReadState(handle, &data) != 0)
            return 0;
        // The left stick works like the D-pad (and repeats like it when held).
        const uint32_t buttons = data.buttons | StickDirection(data.left_x, data.left_y);
        uint32_t pressed = buttons & ~previous;
        previous = buttons;
        constexpr uint32_t kDirections = 0x0010 | 0x0020 | 0x0040 | 0x0080;
        const uint32_t held = buttons & kDirections;
        const double t = Now();
        if (held == 0 || (pressed & kDirections)) {
            repeat_at = t + 0.40;
        } else if (t >= repeat_at) {
            pressed |= held;
            repeat_at = t + 0.09;
        }
        return pressed;
    }
    bool Ok() const {
        return handle >= 0;
    }

private:
    // The stick as one D-pad direction: the larger axis wins (no diagonals); pushed past ~55%
    // it counts, and it lets go below ~30% (so it doesn't flicker at the edge).
    uint32_t StickDirection(uint8_t x, uint8_t y) {
        constexpr uint32_t kUpBit = 0x0010, kRightBit = 0x0020, kDownBit = 0x0040, kLeftBit = 0x0080;
        const int dx = int(x) - 128, dy = int(y) - 128;
        const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        const int amount = ax > ay ? ax : ay;
        if (amount >= 70)
            stick = ax >= ay ? (dx < 0 ? kLeftBit : kRightBit) : (dy < 0 ? kUpBit : kDownBit);
        else if (amount < 38)
            stick = 0;
        return stick;
    }

    void Open() {
        int32_t user = -1;
        if (sceUserServiceGetInitialUser(&user) == 0 && user >= 0)
            handle = scePadOpen(user, 0, 0, nullptr);
    }
    static double Now() {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
    }

    int32_t handle = -1;
    uint32_t previous = ~0u; // ignore buttons already held at start
    int retry = 0;
    double repeat_at = 0;
    uint32_t stick = 0; // the stick's current direction bit
};

[[noreturn]] void ShowResultsForever() {
    WriteSuiteIndex(-1);
    WriteOptions(kSuite[kSuiteCount - 1].options);
    ps4_status_watch_file(kResults, "Benchmark suite results");
    ps4_status_set_stage("Benchmark suite finished - results on the right. Press any button for the launcher.");
    ps4_boot_trace("menu: benchmark suite finished");
    Pad pad;
    for (;;) {
        if (pad.Pressed() != 0) {
            if (FILE* f = fopen(kBackToMenu, "w"))
                fclose(f);
            sceSystemServiceLoadExec("/app0/eboot.bin", nullptr);
        }
        usleep(50 * 1000);
    }
}

} // namespace

extern "C" {

// Shows the start menu, or continues a benchmark suite. Returns when Eden should start; the
// switch files are set for that run.
void ps4_menu_run() {
    // On every return: a program is starting (and the menu sounds' port closes before its audio opens).
    struct MarkStarted { ~MarkStarted() { g_program_started = true; ps4_sound_close(); } } mark_started;
    bool crashed = false;
    g_suite_index = ReadSuiteIndex(&crashed);
    if (g_suite_index >= 0 && crashed) {
        // The previous run never finished: note it and go on with the next one.
        AppendResult(g_suite_index, "crashed (see crash.log)");
        g_suite_index++;
        if (g_suite_index >= kSuiteCount)
            ShowResultsForever();
    }
    if (g_suite_index >= 0) {
        // A few seconds to back out: the suite also resumes when Eden is started by hand after
        // a crash, and nothing else stops it.
        Pad pad;
        bool cancel = false;
        for (int tick = 0; tick < 80 && !cancel && pad.Ok(); tick++) { // 4 s
            char stage[160];
            snprintf(stage, sizeof(stage), "Benchmark suite %d/%d next - press any button to cancel (%d s)",
                     g_suite_index + 1, kSuiteCount, 4 - tick / 20);
            ps4_status_set_stage(stage);
            cancel = pad.Pressed() != 0;
            usleep(50 * 1000);
        }
        if (cancel) {
            WriteSuiteIndex(-1);
            g_suite_index = -1;
            ps4_boot_trace("menu: benchmark suite cancelled");
        }
    }
    if (g_suite_index >= 0) {
        WriteSuiteIndex(g_suite_index, true);
        g_game_path = std::string(kGamesDir) + kBenchmark;
        const SuiteRun& run = kSuite[g_suite_index];
        WriteOptions(run.options);
        char stage[160];
        snprintf(stage, sizeof(stage), "Benchmark suite %d/%d: %s", g_suite_index + 1, kSuiteCount, run.label);
        ps4_status_set_stage(stage);
        ps4_boot_trace(stage);
        return;
    }

    // Home menu "Restart game": straight back into the same game.
    if (remove("/data/switch-emu/autostart") == 0) {
        const std::string chosen = ReadGameChoice();
        struct stat st;
        if (!chosen.empty() && stat((std::string(kGamesDir) + chosen).c_str(), &st) == 0) {
            static std::string title;
            static std::vector<uint32_t> icon;
            title = CachedTitle(chosen);
            icon = CachedIcon(chosen);
            ps4_launcher_set_loading(title.c_str(), icon.empty() ? nullptr : icon.data());
            g_game_path = std::string(kGamesDir) + chosen;
            ps4_boot_trace("menu: restarting the game");
            return;
        }
    }

    Options global = LoadGlobalOptions();
    std::vector<std::string> games = ListGames();
    LauncherPrefs prefs = ReadLauncherPrefs();
    // The chosen order (ties, and games never played, stay by file name: ListGames sorts by it).
    const auto sort_games = [&] {
        std::vector<std::pair<std::string, std::string>> order; // key, file
        for (const std::string& file : games) {
            char key[32] = "";
            if (prefs.sort == kSortRecent)
                snprintf(key, sizeof(key), "%020ld", LONG_MAX - LastPlayed(file));
            else if (prefs.sort == kSortMostPlayed)
                snprintf(key, sizeof(key), "%020ld", LONG_MAX - PlaySeconds(file));
            std::string k = key;
            if (prefs.sort == kSortName)
                for (const char c : CachedTitle(file))
                    k += char(tolower(static_cast<unsigned char>(c)));
            order.emplace_back(k, file);
        }
        std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < games.size(); i++)
            games[i] = order[i].second;
    };
    sort_games();
    int game = 0;
    {
        const std::string chosen = ReadGameChoice();
        for (size_t i = 0; i < games.size(); i++)
            if (games[i] == chosen)
                game = int(i);
    }
    // Library: titles, details and icons (kept alive while the launcher shows them; a new list is
    // handed over before the old one is freed - the drawing thread may be using its icons).
    static std::vector<std::string> titles, details;
    static std::vector<std::vector<uint32_t>> icons;
    const auto publish_library = [&] {
        std::vector<std::string> new_titles, new_details;
        std::vector<std::vector<uint32_t>> new_icons;
        for (const std::string& file : games) {
            new_titles.push_back(CachedTitle(file));
            std::string detail = FileDetail(file) + PlayedDetail(file);
            if (const long played = LastPlayed(file); played > 0)
                if (const std::string when = PlayedWhen(played); !when.empty())
                    detail += "  -  last played " + when;
            new_details.push_back(detail);
            new_icons.push_back(CachedIcon(file));
        }
        std::vector<Ps4LauncherGame> entries;
        for (size_t i = 0; i < games.size(); i++)
            entries.push_back({new_titles[i].c_str(), new_details[i].c_str(),
                               new_icons[i].empty() ? nullptr : new_icons[i].data()});
        ps4_launcher_set_games(entries.data(), int(entries.size()));
        titles.swap(new_titles), details.swap(new_details), icons.swap(new_icons);
    };
    publish_library();

    Pad pad;
    ps4_sound_open(); // already open from the start screen, or opened now (back from a game)
    remove(kBackToMenu);
    Setup setup = CheckSetup();
    const int install_count = CountInstallFiles();
    remove("/data/switch-emu/install-now");
    ps4_launcher_set_info(install_count, pad.Ok());
    if (!setup.Ready())
        ps4_launcher_toast(setup.prod_keys ? "Firmware missing - see Settings > Setup"
                                           : "Keys missing - see Settings > Setup");
    ps4_launcher_show(true);

    enum Row { kGraphics, kMode, kResolution, kGpuAccuracy, kButtons, kSpread, kPin, kMulticore, kFastmem, kAccuracy, kAsync, kProfiler, kBenchmarks, kInstall,
               kBackup, kUseGlobal, kOverlay, kOverlayCorner, kKeys, kFirmware, kLayout, kSort, kAppVersion, kPs4Firmware };
    const std::string app_version = AppVersion(), ps4_firmware = Ps4Firmware();
    int& layout = prefs.layout;
    int shortcut = -1; // row layout: the selected shortcut below the games (-1: a game)
    static const char* const kOverlayModes[] = {"Off", "FPS", "Detailed"};
    static const char* const kCorners[] = {"Top left", "Top right", "Bottom left", "Bottom right"};
    std::string backup_value = "Back up now";
    static const char* const kResolutions[] = {"1x (native)", "0.75x", "0.5x"};
    int tab = games.empty() ? kLauncherSettings : kLauncherGames, row = 0;
    bool start_game = false, start_suite = false, start_install = false;
    bool game_scope = false; // Settings tab: the selected game's own settings
    char install_value[48];
    snprintf(install_value, sizeof(install_value), install_count ? "%d file%s" : "Nothing to install", install_count,
             install_count == 1 ? "" : "s");
    const auto game_opts_path = [&](int g) { return std::string(kTitlesDir) + games[size_t(g)] + ".opts"; };
    Options game_opts = global;
    bool game_has_opts = false;
    int game_opts_for = -1;
    for (;;) {
        // The selected game's own settings (loaded when the selection changes).
        if (!games.empty() && game_opts_for != game) {
            game_opts = global;
            game_has_opts = LoadOptionsFile(game_opts_path(game), game_opts);
            game_opts_for = game;
        }
        if (games.empty())
            game_scope = false;
        Options& o = game_scope ? game_opts : global;
        std::vector<int> ids = {kGraphics, kMode, kResolution, kGpuAccuracy, kButtons, kSpread, kPin, kMulticore, kFastmem,
                                kAccuracy, kAsync, kOverlay, kOverlayCorner, kProfiler};
        if (!game_scope) // the launcher's own setting, for all games
            ids.insert(ids.begin() + 4, {kLayout, kSort});
        if (game_scope)
            ids.push_back(kUseGlobal);
        else
            ids.insert(ids.end(), {kBenchmarks, kInstall, kBackup, kKeys, kFirmware, kAppVersion, kPs4Firmware});
        char keys_value[48], firmware_value[48];
        snprintf(keys_value, sizeof(keys_value), "%s",
                 setup.prod_keys && setup.title_keys ? "Found"
                 : setup.prod_keys                   ? "title.keys missing"
                 : setup.title_keys                  ? "prod.keys missing"
                                                     : "Missing");
        if (setup.firmware_files > 0)
            snprintf(firmware_value, sizeof(firmware_value), "Installed (%d files)", setup.firmware_files);
        else
            snprintf(firmware_value, sizeof(firmware_value), "Missing");
        if (row >= int(ids.size()))
            row = int(ids.size()) - 1;
        std::vector<Ps4LauncherSetting> settings;
        for (const int id : ids) {
            switch (id) {
            case kGraphics:
                settings.push_back({"Graphics", o.graphics ? "Vulkan" : "Off",
                                    "Vulkan draws the game on the PS4's GPU. Off runs without graphics and shows "
                                    "this status screen instead (CPU tests)."});
                break;
            case kMode:
                settings.push_back({"Console mode", o.handheld ? "Handheld" : "Docked",
                                    "Handheld makes games render at 720p instead of 1080p: much less GPU work, "
                                    "often a big speed-up on the PS4. Docked looks sharper."});
                break;
            case kResolution:
                settings.push_back({"Resolution", kResolutions[o.resolution],
                                    "Renders below the game's own resolution for speed (the picture gets softer). "
                                    "1x is what the game asks for."});
                break;
            case kGpuAccuracy:
                settings.push_back({"GPU accuracy", o.gpu_high ? "High" : "Low",
                                    "Low skips some exact GPU synchronisation for speed (what Eden uses on "
                                    "phones). Try High if a game shows missing or flickering graphics."});
                break;
            case kButtons:
                settings.push_back({"Button layout", o.cross_is_a ? "Cross = A" : "Switch positions",
                                    "Switch positions: the buttons sit where a Switch's would (Circle = A, Cross = "
                                    "B). Cross = A: Cross confirms, as on PlayStation (Circle = B, Square = X, "
                                    "Triangle = Y)."});
                break;
            case kSpread:
                settings.push_back({"Spread game threads", o.spread_threads ? "On" : "Off",
                                    "Lets the game's helper threads (music, loading) run on idle CPU cores instead "
                                    "of sharing the main one - fewer lag spikes. Turn off if a game misbehaves."});
                break;
            case kPin:
                settings.push_back({"Pin threads to cores", o.pin_threads ? "On" : "Off",
                                    "Experimental: gives the emulated CPU cores, the GPU thread and the Vulkan "
                                    "worker a PS4 core each instead of letting the system move them around. Try it "
                                    "on and off in the same spot and compare the frame rate."});
                break;
            case kMulticore:
                settings.push_back({"Multi-core", o.multicore ? "On" : "Off",
                                    "Runs the emulated Switch's CPU cores on separate PS4 cores. Faster; turn off "
                                    "only to test a game that misbehaves."});
                break;
            case kFastmem:
                settings.push_back({"Fastmem", o.fastmem ? "On" : "Off",
                                    "Lets recompiled game code reach emulated memory directly. Much faster; off is "
                                    "only for testing."});
                break;
            case kAccuracy:
                settings.push_back({"CPU accuracy", o.cpu_unsafe ? "Unsafe" : "Auto",
                                    "Unsafe skips some exact floating point behaviour for speed. Most games don't "
                                    "notice; switch back to Auto if a game acts strangely."});
                break;
            case kAsync:
                settings.push_back({"Async shaders", o.async_shaders ? "On" : "Off",
                                    "Builds new shaders in the background instead of pausing the game: less "
                                    "stutter, but things can be missing for a moment the first time they appear."});
                break;
            case kProfiler:
                settings.push_back({"Profiler", o.profile ? "On" : "Off",
                                    "Records where the time goes and writes /data/switch-emu/logs/samples.log every 10 s. "
                                    "Slightly slower; leave off for normal play."});
                break;
            case kOverlay:
                settings.push_back({"Performance overlay", kOverlayModes[o.overlay],
                                    "Shows the frame rate in a corner while playing. Detailed adds frame times, how "
                                    "much time is spent waiting for the GPU, new shaders and free GPU memory. Also "
                                    "in the Home menu (hold L3 + R3)."});
                break;
            case kOverlayCorner:
                settings.push_back({"Overlay position", kCorners[o.overlay_corner],
                                    "Which corner of the screen the performance overlay sits in."});
                break;
            case kBenchmarks:
                settings.push_back({"Run all benchmarks", "Start",
                                    "Runs test.nro in 5 configurations, restarting the app between them, and shows "
                                    "the results at the end (bench-results.txt)."});
                break;
            case kInstall:
                settings.push_back({"Install updates and DLC", install_value,
                                    "Installs every .nsp in /data/switch-emu/install (updates and DLC for your games). "
                                    "The files move to install/installed afterwards."});
                break;
            case kBackup:
                settings.push_back({"Back up saves", backup_value.c_str(),
                                    "Copies all save data to /data/switch-emu/save-backups/<date and time>. To "
                                    "restore, copy a backup's folders back into /data/switch-emu/nand/user/save."});
                break;
            case kLayout:
                settings.push_back({"Games layout",
                                    layout == kLayoutRow ? "Console row" : layout == kLayoutList ? "List" : "Grid",
                                    "Grid shows 24 games at a time. Console row shows one row of large tiles "
                                    "with shortcuts below (Down to reach them), like a console's home screen. List "
                                    "shows the games as rows with the selected one large, with its play time."});
                break;
            case kSort:
                settings.push_back({"Sort games", kSortNames[prefs.sort],
                                    "The order of the games tab: the one played last first, the most hours "
                                    "first (play time is counted while a game runs), or by name."});
                break;
            case kAppVersion:
                settings.push_back({"Version", app_version.c_str(),
                                    "PS4 Switch Emu, an unofficial port of the Eden emulator. Please include this "
                                    "version and the PS4 firmware below in bug reports."});
                break;
            case kPs4Firmware:
                settings.push_back({"PS4 firmware", ps4_firmware.c_str(),
                                    "The console's system software version, as the console reports it."});
                break;
            case kKeys:
                settings.push_back({"Keys", keys_value,
                                    "prod.keys and title.keys, dumped from your own Switch, go in "
                                    "/data/switch-emu/keys. Dump them after updating the Switch to the firmware "
                                    "you use here. Cross checks again."});
                break;
            case kFirmware:
                settings.push_back({"Firmware", firmware_value,
                                    "The Switch firmware dumped from your own console: copy its .nca files, loose, "
                                    "into the folder nand/system/Contents/registered inside /data/switch-emu. Cross "
                                    "checks again."});
                break;
            case kUseGlobal:
                settings.push_back({"Use the settings for all games", game_has_opts ? "Reset" : "In use",
                                    "Removes this game's own settings; it then follows the settings for all "
                                    "games again."});
                break;
            }
        }
        // Sections and how each value is drawn.
        for (size_t j = 0; j < ids.size(); j++) {
            switch (ids[j]) {
            case kGraphics: case kMode: case kResolution: case kGpuAccuracy: case kLayout: case kSort:
                settings[j].group = "Display";
                settings[j].kind = kSettingChoice;
                break;
            case kButtons:
                settings[j].group = "Controls";
                settings[j].kind = kSettingChoice;
                break;
            case kAccuracy:
                settings[j].group = "Performance";
                settings[j].kind = kSettingChoice;
                break;
            case kSpread: case kPin: case kMulticore: case kFastmem: case kAsync:
                settings[j].group = "Performance";
                settings[j].kind = kSettingToggle;
                break;
            case kOverlay: case kOverlayCorner:
                settings[j].group = "Debug";
                settings[j].kind = kSettingChoice;
                break;
            case kProfiler:
                settings[j].group = "Debug";
                settings[j].kind = kSettingToggle;
                break;
            case kBenchmarks: case kInstall: case kBackup:
                settings[j].group = "Tools";
                settings[j].kind = kSettingAction;
                break;
            case kKeys: case kFirmware:
                settings[j].group = "Setup";
                settings[j].kind = kSettingText;
                break;
            case kAppVersion: case kPs4Firmware:
                settings[j].group = "About";
                settings[j].kind = kSettingText;
                break;
            case kUseGlobal:
                settings[j].group = "This game";
                settings[j].kind = kSettingAction;
                break;
            }
        }
        ps4_launcher_set_settings(settings.data(), int(settings.size()));
        ps4_launcher_set_state(tab, game, row);
        ps4_launcher_set_layout(layout, shortcut);
        ps4_launcher_set_info(install_count, pad.Ok());
        std::string scope = "All games";
        if (game_scope)
            scope = titles[size_t(game)] + (game_has_opts ? "  (own settings)" : "  (uses all-games settings)");
        ps4_launcher_set_scope(scope.c_str(), !games.empty());

        const uint32_t pressed = pad.Pressed();
        const int old_tab = tab, old_game = game, old_row = row, old_shortcut = shortcut;
        const bool old_scope = game_scope;
        if (pressed & (kL1 | kR1))
            tab = (pressed & kR1) ? kLauncherSettings : kLauncherGames;
        if (tab == kLauncherGames) {
            const int count = int(games.size());
            if (count > 0 && layout == kLayoutRow && shortcut >= 0) {
                // The shortcuts below the row of games.
                if ((pressed & kLeft) && shortcut > 0)
                    shortcut--;
                if ((pressed & kRight) && shortcut + 1 < kShortcutCount)
                    shortcut++;
                if (pressed & (kUp | kCircle))
                    shortcut = -1;
                if (pressed & kCross) {
                    if (shortcut == kShortcutSettings) {
                        tab = kLauncherSettings;
                        shortcut = -1;
                    } else if (shortcut == kShortcutInstall) {
                        if (install_count > 0) {
                            start_install = true;
                            break;
                        }
                        ps4_launcher_toast("Nothing to install: put update/DLC .nsp files in /data/switch-emu/install");
                    } else {
                        backup_value = BackUpSaves();
                        ps4_launcher_toast(("Save backup: " + backup_value).c_str());
                    }
                }
            } else if (count > 0 && layout == kLayoutRow) {
                // One row of games; Down goes to the shortcuts.
                if ((pressed & kLeft) && game > 0)
                    game--;
                if ((pressed & kRight) && game + 1 < count)
                    game++;
                if (pressed & kDown)
                    shortcut = 0;
                if (pressed & (kCross | kOptions)) {
                    start_game = true;
                    break;
                }
                if (pressed & kSquare) { // this game's settings
                    tab = kLauncherSettings;
                    game_scope = true;
                    row = 0;
                }
            } else if (count > 0 && layout == kLayoutList) {
                // Rows: Up/Down one game, Left/Right a page.
                if ((pressed & kUp) && game > 0)
                    game--;
                if ((pressed & kDown) && game + 1 < count)
                    game++;
                if (pressed & kLeft)
                    game = std::max(0, game - 8);
                if (pressed & kRight)
                    game = std::min(count - 1, game + 8);
                if (pressed & (kCross | kOptions)) {
                    start_game = true;
                    break;
                }
                if (pressed & kSquare) { // this game's settings
                    tab = kLauncherSettings;
                    game_scope = true;
                    row = 0;
                }
            } else if (count > 0) {
                // Grid of kLauncherColumns per row (status_screen.cpp draws it).
                if ((pressed & kLeft) && game > 0)
                    game--;
                if ((pressed & kRight) && game + 1 < count)
                    game++;
                if ((pressed & kUp) && game >= kLauncherColumns)
                    game -= kLauncherColumns;
                if ((pressed & kDown) && (game / kLauncherColumns + 1) * kLauncherColumns < count)
                    game = std::min(game + kLauncherColumns, count - 1);
                if (pressed & (kCross | kOptions)) {
                    start_game = true;
                    break;
                }
                if (pressed & kSquare) { // this game's settings
                    tab = kLauncherSettings;
                    game_scope = true;
                    row = 0;
                }
            }
            if ((pressed & kTriangle) && install_count > 0) {
                start_install = true;
                break;
            }
        } else {
            const int rows = int(ids.size());
            if (pressed & kUp)
                row = (row + rows - 1) % rows;
            if (pressed & kDown)
                row = (row + 1) % rows;
            if (pressed & kCircle)
                tab = kLauncherGames;
            if ((pressed & kSquare) && !games.empty()) {
                game_scope = !game_scope;
                row = 0;
            }
            if (pressed & (kCross | kLeft | kRight)) {
                switch (ids[size_t(row)]) {
                case kLayout:
                    layout = (pressed & kLeft) ? (layout + kLayoutCount - 1) % kLayoutCount : (layout + 1) % kLayoutCount;
                    SaveLauncherPrefs(prefs);
                    break;
                case kSort: { // re-sorted now, the same game kept selected
                    prefs.sort = (pressed & kLeft) ? (prefs.sort + kSortCount - 1) % kSortCount : (prefs.sort + 1) % kSortCount;
                    SaveLauncherPrefs(prefs);
                    const std::string selected = games.empty() ? "" : games[size_t(game)];
                    sort_games();
                    publish_library();
                    for (size_t i = 0; i < games.size(); i++)
                        if (games[i] == selected)
                            game = int(i);
                    game_opts_for = -1;
                    break;
                }
                case kGraphics:
                    o.graphics = !o.graphics;
                    break;
                case kMode:
                    o.handheld = !o.handheld;
                    break;
                case kResolution:
                    o.resolution = (pressed & kLeft) ? (o.resolution + 2) % 3 : (o.resolution + 1) % 3;
                    break;
                case kGpuAccuracy:
                    o.gpu_high = !o.gpu_high;
                    break;
                case kButtons:
                    o.cross_is_a = !o.cross_is_a;
                    break;
                case kSpread:
                    o.spread_threads = !o.spread_threads;
                    break;
                case kPin:
                    o.pin_threads = !o.pin_threads;
                    break;
                case kMulticore:
                    o.multicore = !o.multicore;
                    break;
                case kFastmem:
                    o.fastmem = !o.fastmem;
                    break;
                case kAccuracy:
                    o.cpu_unsafe = !o.cpu_unsafe;
                    break;
                case kAsync:
                    o.async_shaders = !o.async_shaders;
                    break;
                case kProfiler:
                    o.profile = !o.profile;
                    break;
                case kOverlay:
                    o.overlay = (pressed & kLeft) ? (o.overlay + 2) % 3 : (o.overlay + 1) % 3;
                    break;
                case kOverlayCorner:
                    o.overlay_corner = (pressed & kLeft) ? (o.overlay_corner + 3) % 4 : (o.overlay_corner + 1) % 4;
                    break;
                case kBenchmarks:
                    start_suite = (pressed & kCross) != 0;
                    break;
                case kInstall:
                    start_install = (pressed & kCross) && install_count > 0;
                    break;
                case kBackup:
                    if (pressed & kCross) {
                        backup_value = BackUpSaves();
                        ps4_launcher_toast(("Save backup: " + backup_value).c_str());
                    }
                    break;
                case kKeys: case kFirmware:
                    if (pressed & kCross) {
                        setup = CheckSetup();
                        ps4_launcher_toast(setup.Ready() ? "Keys and firmware found" : "Still missing - see the help text");
                    }
                    break;
                case kUseGlobal:
                    if (pressed & kCross) {
                        remove(game_opts_path(game).c_str());
                        game_opts_for = -1; // reload: back to the global settings
                        ps4_launcher_toast((titles[size_t(game)] + " uses the settings for all games").c_str());
                    }
                    break;
                default:
                    break;
                }
                // Keep choices even if Eden is closed from here.
                if (game_scope && ids[size_t(row)] != kUseGlobal) {
                    mkdir(kTitlesDir, 0777);
                    SaveOptionsFile(game_opts_path(game), game_opts);
                    if (!game_has_opts)
                        ps4_launcher_toast((titles[size_t(game)] + " now has its own settings").c_str());
                    game_has_opts = true;
                } else if (!game_scope) {
                    SaveOptionsFile(kGlobalOptions, global);
                    if (game_opts_for >= 0 && !game_has_opts)
                        game_opts_for = -1; // a game without its own settings follows these
                }
                if (start_suite || start_install)
                    break;
            }
        }
        // Sounds: moving around ticks, a setting changing plucks, a choice chimes, back falls.
        if ((pressed & kCircle) && old_tab == kLauncherSettings)
            ps4_sound_play(kSoundBack);
        else if (tab == kLauncherSettings && old_tab == tab && (pressed & (kLeft | kRight)))
            ps4_sound_play(kSoundToggle);
        else if (tab == kLauncherSettings && old_tab == tab && (pressed & kCross))
            ps4_sound_play(kSoundConfirm);
        else if (tab == kLauncherGames && old_shortcut >= 0 && (pressed & kCross))
            ps4_sound_play(kSoundConfirm);
        else if (tab != old_tab || game != old_game || row != old_row || game_scope != old_scope ||
                 shortcut != old_shortcut)
            ps4_sound_play(kSoundMove);
        usleep(16 * 1000);
    }
    ps4_sound_play(start_game ? kSoundLaunch : kSoundConfirm);
    if (start_game && !games.empty()) {
        // The card zooms into the loading screen (also lets the chime finish).
        ps4_launcher_set_loading(titles[size_t(game)].c_str(), icons[size_t(game)].empty() ? nullptr : icons[size_t(game)].data());
        ps4_launcher_launch();
    } else {
        usleep(180 * 1000); // let the confirm chime finish before the port closes
    }
    // What this run uses: the started game's own settings, or the global ones.
    Options o = global;
    if (start_game && !games.empty())
        LoadOptionsFile(game_opts_path(game), o);
    ps4_launcher_show(false);
    if (start_suite) {
        remove(kResults);
        WriteSuiteIndex(0, true);
        g_suite_index = 0;
        WriteOptions(kSuite[0].options);
        ps4_status_set_stage("Benchmark suite 1/5: starting");
        ps4_boot_trace("menu: benchmark suite started");
        return;
    }
    if (start_install) {
        if (FILE* f = fopen("/data/switch-emu/install-now", "w"))
            fclose(f);
        ps4_status_set_stage("Installing updates and DLC...");
        ps4_boot_trace("menu: installing updates/DLC");
    }
    if (start_game && !games.empty())
        MarkPlayed(games[size_t(game)]);
    if (start_game && !games.empty())
        ps4_launcher_set_loading(titles[size_t(game)].c_str(), icons[size_t(game)].empty() ? nullptr : icons[size_t(game)].data());
    WriteOptions(o);
    if (!games.empty()) {
        WriteGameChoice(games[size_t(game)]);
        g_game_path = std::string(kGamesDir) + games[size_t(game)];
    }
    ps4_status_set_menu(nullptr, 0);
    char line[160];
    snprintf(line, sizeof(line), "menu: start %.60s (multi-core %d, fastmem %d, cpu-unsafe %d, graphics %d)",
             g_game_path.c_str(), o.multicore, o.fastmem, o.cpu_unsafe, o.graphics);
    ps4_boot_trace(line);
}

// The guest program exited. In a suite: record this run, then ask for a restart into the next one
// (done by ps4_menu_poll_restart on the main thread).
void ps4_menu_guest_exited() {
    // Eden calls the exit callback more than once.
    static bool s_handled = false;
    if (s_handled)
        return;
    s_handled = true;
    if (g_suite_index < 0) {
        // A program finished: back to the start menu. With Vulkan the status screen can't show
        // over the swap chain, so without this the last frame just stays up.
        if (FILE* f = fopen(kBackToMenu, "w"))
            fclose(f);
        ps4_status_set_stage("The program exited - back to the menu...");
        g_restart_requested = true;
        return;
    }
    usleep(800 * 1000); // let the status screen take in the benchmark's last lines
    char text[4096];
    ps4_status_copy_watch(text, sizeof(text));
    char integer[16], floating[16], memory[16], heap[16];
    FindResult(text, "integer", integer, sizeof(integer));
    FindResult(text, "float", floating, sizeof(floating));
    FindResult(text, "memory", memory, sizeof(memory));
    FindResult(text, "heap", heap, sizeof(heap));
    char summary[96];
    snprintf(summary, sizeof(summary), "int %s, float %s, mem %s, heap %s", integer, floating, memory, heap);
    AppendResult(g_suite_index, summary);
    const int next = g_suite_index + 1;
    if (next < kSuiteCount) {
        WriteSuiteIndex(next);
        g_restart_requested = true;
        ps4_status_set_stage("Benchmark suite: restarting for the next run...");
    } else {
        WriteSuiteIndex(-1);
        WriteOptions(kSuite[kSuiteCount - 1].options);
        ps4_status_watch_file(kResults, "Benchmark suite results");
        ps4_status_set_stage("Benchmark suite finished - results on the right. Press any button for the launcher.");
        ps4_boot_trace("menu: benchmark suite finished");
        g_wait_for_button = true;
    }
}

// The program to run (chosen in the menu; test.nro for the benchmark suite).
const char* ps4_menu_game_path() {
    return g_game_path.c_str();
}

// The program couldn't be loaded (Core::SystemResultStatus): say so, note it in the suite
// results, and restart to the menu or the next suite run. Doesn't return.
void ps4_menu_load_failed(int code, const char* path) {
    char line[320];
    snprintf(line, sizeof(line), "Could not load %s (error %d%s) - back to the menu in 5 s", path, code,
             code == 2 ? ": file missing or not a Switch program" : "");
    ps4_status_set_stage(line);
    ps4_boot_trace(line);
    if (g_suite_index >= 0) {
        char summary[64];
        snprintf(summary, sizeof(summary), "load failed (error %d)", code);
        AppendResult(g_suite_index, summary);
        const int next = g_suite_index + 1;
        WriteSuiteIndex(next < kSuiteCount ? next : -1);
    } else if (FILE* f = fopen(kBackToMenu, "w")) {
        fclose(f);
    }
    sleep(5);
    sceSystemServiceLoadExec("/app0/eboot.bin", nullptr);
    for (;;)
        sleep(1);
}

// After an install run: show the result, then back to the menu. Doesn't return.
void ps4_menu_restart_to_menu(const char* message) {
    ps4_status_set_stage(message);
    ps4_boot_trace(message);
    if (FILE* f = fopen(kBackToMenu, "w"))
        fclose(f);
    sleep(6);
    sceSystemServiceLoadExec("/app0/eboot.bin", nullptr);
    for (;;)
        sleep(1);
}

// Back to the launcher at once (in-game L3+R3). Doesn't return.
void ps4_menu_restart_now() {
    if (FILE* f = fopen(kBackToMenu, "w"))
        fclose(f);
    sceSystemServiceLoadExec("/app0/eboot.bin", nullptr);
    for (;;)
        sleep(1);
}

// Called regularly from the main thread: performs a requested restart.
// Back to the launcher: the app restarts into it (as "Quit to launcher" does).
static void BackToLauncher(const char* why) {
    if (FILE* f = fopen(kBackToMenu, "w"))
        fclose(f);
    ps4_boot_trace(why);
    sceSystemServiceLoadExec("/app0/eboot.bin", nullptr);
}

// The main loop's newly pressed buttons while the activity screen shows (no game on screen):
// Circle cancels a benchmark suite run; any button leaves a finished suite's results.
void ps4_menu_activity_input(bool any, bool circle) {
    if (g_wait_for_button && any) {
        BackToLauncher("menu: benchmark results closed");
    } else if (g_suite_index >= 0 && circle) {
        WriteSuiteIndex(-1);
        WriteOptions(kSuite[kSuiteCount - 1].options);
        ps4_status_set_stage("Benchmark suite cancelled - back to the launcher...");
        BackToLauncher("menu: benchmark suite cancelled");
    }
}

// After a crash (crashlog.cpp's signal handler, so only system calls): back to the launcher, or
// into the next benchmark run when a suite was running (it notes the crash and goes on).
void ps4_menu_after_crash() {
    if (!g_program_started)
        return;
    if (g_suite_index < 0) {
        const int fd = open(kBackToMenu, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0)
            close(fd);
    }
    sceSystemServiceLoadExec("/app0/eboot.bin", nullptr);
}

void ps4_menu_poll_restart() {
    if (!g_restart_requested)
        return;
    g_restart_requested = false;
    ps4_boot_trace(g_suite_index >= 0 ? "menu: restarting for the next benchmark run"
                                      : "menu: program exited, restarting to the menu");
    sceSystemServiceLoadExec("/app0/eboot.bin", nullptr);
}

} // extern "C"
