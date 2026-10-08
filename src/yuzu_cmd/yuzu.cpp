// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <iostream>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include "common/settings_enums.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL_main.h>

#include <fmt/ostream.h>

#include "common/logging.h"
#include "common/param_package.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "common/string_util.h"
#include "common/sparse_large_vector.h"
#include "core/core.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/perf_stats.h"
#include "core/core_timing.h"
#include "core/cpu_manager.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/device_memory.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "frontend_common/config.h"
#include <algorithm>
#include <strings.h>
#include <dirent.h>
#include "frontend_common/content_manager.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "input_common/main.h"
#include "input_common/drivers/virtual_gamepad.h"
#include <cmath>
#include <fstream>
#include <sstream>
#include <thread>
#include "network/network.h"
#include "sdl_config.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"
#include "core/frontend/framebuffer_layout.h"
#include "yuzu_cmd/emu_window/emu_window_sdl3.h"
#ifdef HAS_OPENGL
#include "yuzu_cmd/emu_window/emu_window_sdl3_gl.h"
#endif
#include "yuzu_cmd/emu_window/emu_window_sdl3_null.h"
#include "yuzu_cmd/emu_window/emu_window_sdl3_vk.h"

#ifdef _WIN32
// windows.h needs to be included before shellapi.h
#include <windows.h>
#include <shellapi.h>
#include "common/windows/timer_resolution.h"
#endif

#undef _UNICODE
#include <getopt.h>
#ifndef _MSC_VER
#include <unistd.h>
#endif

#if !defined(__OPENORBIS__)
#define PS4_TRACE(...) ((void)0)
#define PS4_STAGE(...) ((void)0)
#endif
#if defined(__OPENORBIS__)
#include <orbis/libkernel.h>
#include <orbis/SystemService.h>
#include <orbis/AudioOut.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <sys/stat.h>
#include <pthread.h>
#include <pthread_np.h>
extern "C" void ps4_boot_trace(const char* stage); // externals/ps4sup/crashlog.cpp
// externals/ps4sup/dps4/ps4_runtime.cpp (DolphinPS4)
extern "C" int ps4_exec_probe();
extern "C" const char* ps4rt_heap_source(void);
extern "C" size_t ps4rt_heap_size(void);
#define PS4_TRACE(...) ps4_boot_trace(fmt::format(__VA_ARGS__).c_str())
// externals/ps4sup/status_screen.cpp: on-screen status while there is no renderer.
extern "C" void ps4_status_start(const char* title);
extern "C" void ps4_status_stop();
extern "C" void ps4_status_set_stage(const char* stage);
extern "C" void ps4_status_watch_file(const char* path, const char* heading);
// externals/ps4sup/ps4_menu.cpp: start menu (DualShock 4) and the automatic benchmark suite.
extern "C" void ps4_menu_run();
extern "C" void ps4_menu_guest_exited();
extern "C" void ps4_menu_poll_restart();
extern "C" void ps4_menu_load_failed(int code, const char* path);
extern "C" void ps4_sampler_start(); // externals/ps4sup/sampler.cpp
extern "C" void ps4_menu_restart_to_menu(const char* message);
extern "C" void ps4_menu_restart_now();
extern "C" bool ps4_swapchain_overlay_begin();
extern "C" void ps4_swapchain_overlay_draw(void (*draw)(uint32_t*, const uint32_t*, int, void*), void* ctx);
extern "C" void ps4_swapchain_overlay_end();
extern "C" bool ps4_status_home_animating(); // status_screen.cpp
extern "C" unsigned long long ps4_gpu_wait_us(); // vk_swapchain.cpp
extern "C" void ps4_overlay_configure(int mode, int corner); // vk_present_manager.cpp
extern "C" int ps4_overlay_mode();
extern "C" void ps4_swapchain_show_loading(const char* progress); // vk_swapchain.cpp
#define PS4_STAGE(...) ps4_status_set_stage(fmt::format(__VA_ARGS__).c_str())

// The launcher's library: a game's real title and icon, read from its file into
// /data/switch-emu/titles/<file>.txt and .jpg (what ps4_menu.cpp shows).
static void PS4CacheTitle(Core::System& system, const std::string& filepath) {
    const auto file = system.GetFilesystem()->OpenFile(filepath, FileSys::OpenMode::Read);
    if (!file)
        return;
    const auto loader = Loader::GetLoader(system, file);
    if (!loader)
        return;
    const std::string base = filepath.substr(filepath.rfind('/') + 1);
    mkdir("/data/switch-emu/titles", 0777);
    std::string title;
    if (loader->ReadTitle(title) == Loader::ResultStatus::Success && !title.empty()) {
        if (FILE* f = fopen(("/data/switch-emu/titles/" + base + ".txt").c_str(), "w")) {
            fputs(title.c_str(), f);
            fclose(f);
        }
    }
    std::vector<u8> icon;
    if (loader->ReadIcon(icon) == Loader::ResultStatus::Success && !icon.empty()) {
        if (FILE* f = fopen(("/data/switch-emu/titles/" + base + ".jpg").c_str(), "wb")) {
            fwrite(icon.data(), 1, icon.size(), f);
            fclose(f);
        }
    }
    PS4_TRACE("library: {}: \"{}\", icon {} bytes", base, title, icon.size());
}

// Before the launcher: reads the title and icon of every game (.nsp, .xci) not read yet, so the
// library shows them before a game's first start. Reading a file runs behind a marker
// (titles/<file>.scanning); if it crashed the app, the next start marks that file .unreadable and
// skips it from then on.
static void PS4ScanLibrary(Core::System& system) {
    constexpr const char* kGames = "/data/switch-emu/games/";
    constexpr const char* kTitles = "/data/switch-emu/titles/";
    struct stat st;
    if (stat("/data/switch-emu/keys/prod.keys", &st) != 0)
        return; // .nsp and .xci can't be read without the keys
    std::vector<std::string> todo;
    if (DIR* dir = opendir(kGames)) {
        while (dirent* entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name.size() < 5)
                continue;
            std::string ext = name.substr(name.size() - 4);
            for (char& c : ext)
                c = char(tolower(c));
            if (ext != ".nsp" && ext != ".xci")
                continue;
            const std::string base = kTitles + name;
            if (stat((base + ".scanning").c_str(), &st) == 0) {
                rename((base + ".scanning").c_str(), (base + ".unreadable").c_str());
                PS4_TRACE("library: {} crashed the app while being read; skipped from now on", name);
                continue;
            }
            if (stat((base + ".txt").c_str(), &st) != 0 && stat((base + ".unreadable").c_str(), &st) != 0)
                todo.push_back(name);
        }
        closedir(dir);
    }
    if (todo.empty())
        return;
    std::sort(todo.begin(), todo.end());
    mkdir(kTitles, 0777);
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
    for (size_t i = 0; i < todo.size(); i++) {
        PS4_STAGE("Reading game info ({} of {})", i + 1, todo.size());
        const std::string marker = kTitles + todo[i] + ".scanning";
        if (FILE* f = fopen(marker.c_str(), "w"))
            fclose(f);
        try {
            PS4CacheTitle(system, kGames + todo[i]);
        } catch (const std::exception& e) {
            PS4_TRACE("library: {}: {}", todo[i], e.what());
        }
        remove(marker.c_str());
    }
}

// Menu "Install updates/DLC": installs every .nsp in /data/switch-emu/install into the emulated user
// NAND (Eden applies updates and DLC from there when the base game starts), moves the installed
// files to /data/switch-emu/install/installed/, then restarts to the menu. Doesn't return.
static void PS4InstallPending(Core::System& system) {
    constexpr const char* kDir = "/data/switch-emu/install";
    std::vector<std::string> files;
    if (DIR* dir = opendir(kDir)) {
        while (dirent* entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name.size() > 4 && strcasecmp(name.c_str() + name.size() - 4, ".nsp") == 0)
                files.push_back(name);
        }
        closedir(dir);
    }
    std::sort(files.begin(), files.end());
    mkdir("/data/switch-emu/install/installed", 0777);
    int ok = 0, failed = 0, base = 0;
    for (size_t i = 0; i < files.size(); i++) {
        const std::string path = std::string(kDir) + "/" + files[i];
        int last_percent = -1;
        const auto progress = [&](size_t total, size_t done) {
            const int percent = total ? int(done * 100 / total) : 0;
            if (percent != last_percent) {
                last_percent = percent;
                PS4_STAGE("Installing {} ({}/{}): {}%", files[i], i + 1, files.size(), percent);
            }
            return false; // never cancel
        };
        const auto result = ContentManager::InstallNSP(system, *system.GetFilesystem(), path, progress);
        const char* what = "failed";
        switch (result) {
        case ContentManager::InstallResult::Success:
        case ContentManager::InstallResult::Overwrite:
            what = result == ContentManager::InstallResult::Success ? "installed" : "reinstalled";
            ok++;
            rename(path.c_str(), (std::string(kDir) + "/installed/" + files[i]).c_str());
            break;
        case ContentManager::InstallResult::BaseInstallAttempted:
            what = "is a base game - put it in /data/switch-emu/games instead";
            base++;
            break;
        default:
            failed++;
            break;
        }
        PS4_TRACE("install: {} {}", files[i], what);
    }
    char summary[200];
    snprintf(summary, sizeof(summary),
             "Install finished: %d installed, %d failed, %d base games (belong in /data/switch-emu/games). Back to the menu...",
             ok, failed, base);
    ps4_menu_restart_to_menu(summary);
}
extern "C" const char* ps4_menu_game_path();
#elif defined(_WIN32)
extern "C" {
// tells Nvidia and AMD drivers to use the dedicated GPU by default on laptops with switchable
// graphics
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

static void PrintHelp(const char* argv0) {
    std::cout << "Usage: " << argv0
              << " [options] <filename>\n"
                 "-c, --config          Load the specified configuration file\n"
                 "-f, --fullscreen      Start in fullscreen mode\n"
                 "-g, --game            File path of the game to load\n"
                 "-h, --help            Display this help and exit\n"
                 "-m, --multiplayer=nick:password@address:port"
                 " Nickname, password, address and port for multiplayer\n"
                 "-p, --program         Pass following string as arguments to executable\n"
                 "-u, --user            Select a specific user profile from 0 to 7\n"
                 "-d, --debug           Run the GDB stub on a port from 1 to 65535\n"
                 "-v, --version         Output version information and exit\n";
}

static void PrintVersion() {
    std::cout << "Eden " << Common::g_scm_branch << " " << Common::g_scm_desc << std::endl;
}

static void OnStateChanged(const Network::RoomMember::State& state) {
    switch (state) {
    case Network::RoomMember::State::Idle:
        LOG_DEBUG(Network, "Network is idle");
        break;
    case Network::RoomMember::State::Joining:
        LOG_DEBUG(Network, "Connection sequence to room started");
        break;
    case Network::RoomMember::State::Joined:
        LOG_DEBUG(Network, "Successfully joined to the room");
        break;
    case Network::RoomMember::State::Moderator:
        LOG_DEBUG(Network, "Successfully joined the room as a moderator");
        break;
    default:
        break;
    }
}

static void OnNetworkError(const Network::RoomMember::Error& error) {
    switch (error) {
    case Network::RoomMember::Error::LostConnection:
        LOG_DEBUG(Network, "Lost connection to the room");
        break;
    case Network::RoomMember::Error::CouldNotConnect:
        LOG_ERROR(Network, "Error: Could not connect");
        exit(1);
        break;
    case Network::RoomMember::Error::NameCollision:
        LOG_ERROR(
            Network,
            "You tried to use the same nickname as another user that is connected to the Room");
        exit(1);
        break;
    case Network::RoomMember::Error::IpCollision:
        LOG_ERROR(Network, "You tried to use the same fake IP-Address as another user that is "
                           "connected to the Room");
        exit(1);
        break;
    case Network::RoomMember::Error::WrongPassword:
        LOG_ERROR(Network, "Room replied with: Wrong password");
        exit(1);
        break;
    case Network::RoomMember::Error::WrongVersion:
        LOG_ERROR(Network,
                  "You are using a different version than the room you are trying to connect to");
        exit(1);
        break;
    case Network::RoomMember::Error::RoomIsFull:
        LOG_ERROR(Network, "The room is full");
        exit(1);
        break;
    case Network::RoomMember::Error::HostKicked:
        LOG_ERROR(Network, "You have been kicked by the host");
        break;
    case Network::RoomMember::Error::HostBanned:
        LOG_ERROR(Network, "You have been banned by the host");
        break;
    case Network::RoomMember::Error::UnknownError:
        LOG_ERROR(Network, "UnknownError");
        break;
    case Network::RoomMember::Error::PermissionDenied:
        LOG_ERROR(Network, "PermissionDenied");
        break;
    case Network::RoomMember::Error::NoSuchUser:
        LOG_ERROR(Network, "NoSuchUser");
        break;
    }
}

static void OnMessageReceived(const Network::ChatEntry& msg) {
    std::cout << std::endl << msg.nickname << ": " << msg.message << std::endl << std::endl;
}

static void OnStatusMessageReceived(const Network::StatusMessageEntry& msg) {
    std::string message = [&]() {
        switch (msg.type) {
        case Network::IdMemberJoin:
            return fmt::format("{} has joined", msg.nickname);
        case Network::IdMemberLeave:
            return fmt::format("{} has left", msg.nickname);
        case Network::IdMemberKicked:
            return fmt::format("{} has been kicked", msg.nickname);
        case Network::IdMemberBanned:
            return fmt::format("{} has been banned", msg.nickname);
        case Network::IdAddressUnbanned:
            return fmt::format("{} has been unbanned", msg.nickname);
        default:
            return std::string{};
        }
    }();
    if (!message.empty())
        std::cout << std::endl << "* " << message << std::endl << std::endl;
}

// EDEN_AUTOPRESS=<file>: player 1 becomes a virtual controller that presses buttons from a script,
// so a game can be driven without a controller (profiling gameplay on a PC, scripts/profile-pc.sh).
// One instruction per line, times in seconds from the start, '#' starts a comment:
//   at <time> <button> [hold ms]
//   every <period> <button> [hold ms] from <time> [until <time>]
//   hold <button> from <time> until <time>
// Buttons: A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN.
static void StartAutopress(InputCommon::InputSubsystem& input, Core::HID::EmulatedController* controller,
                           const char* path) {
    static const char* const kNames[] = {"A", "B", "X", "Y", "", "", "L", "R", "ZL", "ZR",
                                         "PLUS", "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};
    constexpr int kButtons = 16; // Settings::NativeButton and VirtualButton agree up to DDown
    for (int b = 0; b < kButtons; b++) {
        Common::ParamPackage param;
        param.Set("engine", "virtual_gamepad");
        param.Set("port", 0);
        param.Set("button", b);
        controller->SetButtonParam(b, param);
    }
    controller->SaveCurrentConfig(); // as for a real controller: settings reloads keep the mapping
    controller->ReloadFromSettings();
    struct Press {
        double from, until, period, hold;
        int button;
    };
    std::vector<Press> presses;
    std::ifstream file(path);
    std::string line;
    const auto button_of = [&](const std::string& name) {
        for (int b = 0; b < kButtons; b++)
            if (name == kNames[b])
                return b;
        return -1;
    };
    while (std::getline(file, line)) {
        line = line.substr(0, line.find('#'));
        std::istringstream in(line);
        std::string kind, name, word;
        if (!(in >> kind))
            continue;
        Press p{0, 0, 0, 0.1, -1};
        if (kind == "at") {
            in >> p.from >> name;
            if (in >> word)
                p.hold = std::stod(word) / 1000.0;
            p.until = p.from + p.hold;
        } else if (kind == "every") {
            in >> p.period >> name;
            p.until = 1e9;
            while (in >> word) {
                if (word == "from")
                    in >> p.from;
                else if (word == "until")
                    in >> p.until;
                else
                    p.hold = std::stod(word) / 1000.0;
            }
        } else if (kind == "hold") {
            in >> name >> word >> p.from >> word >> p.until;
            p.hold = p.until - p.from;
        }
        p.button = button_of(name);
        if (p.button < 0) {
            LOG_ERROR(Input, "autopress: can't read \"{}\"", line);
            continue;
        }
        presses.push_back(p);
    }
    LOG_INFO(Input, "autopress: {} instructions from {}", presses.size(), path);
    std::thread([pad = input.GetVirtualGamepad(), controller, presses] {
        const auto start = std::chrono::steady_clock::now();
        bool down[kButtons] = {};
        for (;;) {
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            bool want[kButtons] = {};
            for (const Press& p : presses) {
                if (now < p.from || now >= p.until)
                    continue;
                want[p.button] |= p.period > 0 ? std::fmod(now - p.from, p.period) < p.hold : true;
            }
            for (int b = 0; b < kButtons; b++)
                if (want[b] != down[b]) {
                    pad->SetButtonState(0, b, down[b] = want[b]);
                    if (std::getenv("EDEN_AUTOPRESS_TRACE"))
                        LOG_WARNING(Input, "autopress: {:.2f} s {} {} (controller sees {}, connected {})", now,
                                    kNames[b], want[b] ? "down" : "up", controller->GetButtonsValues()[b].value,
                                    controller->IsConnected());
                }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }).detach();
}

struct SdlState {
    Core::System system{};
    std::unique_ptr<EmuWindow_SDL3> emu_window;
};

extern "C" SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv) {
#ifdef __OPENORBIS__
    {
        pthread_attr_t attr;
        void* stack_addr = nullptr;
        size_t stack_size = 0;
        int here = 0;
        pthread_attr_init(&attr);
        pthread_attr_get_np(pthread_self(), &attr);
        pthread_attr_getstack(&attr, &stack_addr, &stack_size);
        pthread_attr_destroy(&attr);
        PS4_TRACE("main: SDL_AppInit on stack {} + {} KiB, sp {} ({} KiB left)", stack_addr, stack_size >> 10,
                  static_cast<void*>(&here), (uintptr_t(&here) - uintptr_t(stack_addr)) >> 10);
    }
#endif
    PS4_TRACE("main: SDL_AppInit, constructing Core::System");
    SdlState* state = new SdlState();
    // Handed back to SDL_AppIterate/AppEvent/AppQuit (was never stored: they got nullptr).
    *appstate = state;
    PS4_TRACE("main: Core::System constructed");

#ifdef _WIN32
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "wb", stdout);
        freopen("CONOUT$", "wb", stderr);
    }
#endif
#ifdef __OPENORBIS__
    // May prevent spurious crashes on swap handlers...
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    ps4_status_start("PS4 Switch Emu");
    // A fresh run: drop the previous run's output so its lines aren't taken as already seen.
    remove("/data/switch-emu/sdmc/cpu-benchmark.txt");
    ps4_status_watch_file("/data/switch-emu/sdmc/cpu-benchmark.txt", "test.nro output");
    PS4_STAGE("Starting up");
    PS4_TRACE("main: eden-cli starting, heap {} MiB from {}", ps4rt_heap_size() >> 20, ps4rt_heap_source());
    // How this jailbreak lets us map JIT code (0 = plain RWX mmap ... 4 = JIT shared memory,
    // -1 = none works); dynarmic's PS4ExecAllocator maps through the method found here.
    PS4_TRACE("main: executable memory method {}", ps4_exec_probe());
    if (struct stat st; stat("/data/switch-emu/test-crash", &st) == 0) {
        // Test switch: checks that crash.log gets written.
        PS4_TRACE("main: test-crash: writing to address 0x10");
        *reinterpret_cast<volatile int*>(0x10) = 1;
    }
    if (struct stat st; stat("/data/switch-emu/skip-userservice", &st) == 0) {
        PS4_TRACE("main: skip-userservice: not calling sceUserServiceInitialize");
    } else {
        PS4_TRACE("main: calling sceUserServiceInitialize");
        PS4_TRACE("main: user service init: {:#x}", u32(sceUserServiceInitialize(nullptr)));
    }
    Common::InitSwap();
    PS4_TRACE("main: swap handler installed");
    // Titles and icons for games added since the last start, then the launcher (or the next run
    // of a benchmark suite); the launcher sets the switch files read below.
    PS4ScanLibrary(state->system);
    ps4_menu_run();
#endif

    Common::Log::Initialize();
    Common::Log::SetColorConsoleBackendEnabled(true);
    Common::Log::Start();

    int option_index = 0;
#ifdef _WIN32
    int argc_w;
    auto argv_w = CommandLineToArgvW(GetCommandLineW(), &argc_w);
    if (argv_w == nullptr) {
        LOG_CRITICAL(Frontend, "Failed to get command line arguments");
        return SDL_APP_FAILURE;
    }
#endif
    std::string filepath;
    std::optional<std::string> config_path{};
    std::string program_args;
    std::optional<int> selected_user{};
    std::optional<u16> override_gdb_port{};
    bool use_multiplayer = false;
    // Platforms that start with fullscreen
#if defined(__OPENORBIS__) || defined(__ANDROID__)
    bool fullscreen = true;
#else
    bool fullscreen = false;
#endif
    bool force_null_render = false;
    bool force_single_core = false;
    std::string nickname{};
    std::string password{};
    std::string address{};
    std::string input_profile{};
    std::optional<std::string> log_filter{};
    u16 port = Network::DefaultRoomPort;

    static struct option long_options[] = {
        // clang-format off
        {"debug", no_argument, 0, 'd'},
        {"config", required_argument, 0, 'c'},
        {"fullscreen", no_argument, 0, 'f'},
        {"help", no_argument, 0, 'h'},
        {"game", required_argument, 0, 'g'},
        {"multiplayer", required_argument, 0, 'm'},
        {"program", optional_argument, 0, 'p'},
        {"user", required_argument, 0, 'u'},
        {"version", no_argument, 0, 'v'},
        {"input-profile", no_argument, 0, 'i'},
        {"null-render", no_argument, 0, 'n'},
        {"singlecore", no_argument, 0, 's'},
        {"filter", no_argument, 0, 'x'},
        {0, 0, 0, 0},
        // clang-format on
    };
#ifdef __OPENORBIS__
    // PS4 will use this path by default UNLESS overriden; this is so users
    // can quickly launch whatever they want.
    filepath = ps4_menu_game_path(); // chosen in the start menu (default test.nro)
#endif
    while (optind < argc) {
        int arg = getopt_long(argc, argv, "g:fhvcip::c:u:d:", long_options, &option_index);
        if (arg != -1) {
            switch (char(arg)) {
            case 'd':
                override_gdb_port = uint16_t(atoi(optarg));
                break;
            case 'c':
                config_path = optarg;
                break;
            case 'f':
                fullscreen = true;
                LOG_INFO(Frontend, "Starting in fullscreen mode...");
                break;
            case 'h':
                PrintHelp(argv[0]);
                return SDL_APP_FAILURE;
            case 'g':
                filepath = std::string(optarg);
                break;
            case 'i': {
                input_profile = std::string(optarg);
                break;
            }
            case 'm': {
                use_multiplayer = true;
                const std::string str_arg(optarg);
                // regex to check if the format is nickname:password@ip:port
                // with optional :password
                const std::regex re("^([^:]+)(?::(.+))?@([^:]+)(?::([0-9]+))?$");
                if (!std::regex_match(str_arg, re)) {
                    std::cout << "Wrong format for option --multiplayer\n";
                    PrintHelp(argv[0]);
                    return SDL_APP_FAILURE;
                }

                std::smatch match;
                std::regex_search(str_arg, match, re);
                ASSERT(match.size() == 5);
                nickname = match[1];
                password = match[2];
                address = match[3];
                if (!match[4].str().empty()) {
                    port = u16(std::strtoul(match[4].str().c_str(), nullptr, 0));
                }
                std::regex nickname_re("^[a-zA-Z0-9._\\- ]+$");
                if (!std::regex_match(nickname, nickname_re)) {
                    LOG_ERROR(Frontend, "Nickname is not valid. Must be 4 to 20 alphanumeric characters");
                    return SDL_APP_FAILURE;
                }
                if (address.empty()) {
                    LOG_ERROR(Frontend, "Address to room must not be empty");
                    return SDL_APP_FAILURE;
                }
                break;
            }
            case 'p':
                program_args = argv[optind];
                ++optind;
                break;
            case 'u':
                selected_user = atoi(optarg);
                break;
            case 'v':
                PrintVersion();
                return SDL_APP_FAILURE;
            case 'n':
                force_null_render = true;
                break;
            case 's':
                force_single_core = true;
                break;
            case 'x':
                log_filter = argv[optind];
                ++optind;
                break;
            }
        } else {
#ifdef _WIN32
            filepath = Common::UTF16ToUTF8(argv_w[optind]);
#else
            filepath = argv[optind];
#endif
            optind++;
        }
    }

    SdlConfig config{config_path};

    // apply the log_filter setting
    // the logger was initialized before and doesn't pick up the filter on its own
    Common::Log::Filter filter;
    filter.ParseFilterString(log_filter.value_or(Settings::values.log_filter.GetValue()));
    Common::Log::SetGlobalFilter(filter);

    if (!program_args.empty()) {
        Settings::values.program_args = program_args;
    }

    if (!input_profile.empty()) {
        auto& players = Settings::values.players.GetValue();
        players[0].profile_name = input_profile;
    }

    if (selected_user.has_value()) {
        Settings::values.current_user = std::clamp(*selected_user, 0, 7);
    }

    if (override_gdb_port.has_value()) {
        Settings::values.use_gdbstub = true;
        Settings::values.gdbstub_port = *override_gdb_port;
    }

    if (force_single_core) {
        Settings::values.use_multi_core = false;
    }

    if (force_null_render) {
        Settings::values.renderer_backend = Settings::RendererBackend::Null;
    }

#ifdef _WIN32
    LocalFree(argv_w);
#endif

    if (filepath.empty()) {
        LOG_CRITICAL(Frontend, "Failed to load ROM: No ROM specified");
        return SDL_APP_FAILURE;
    }

#ifdef __OPENORBIS__
    // Test switches, applied before System::Initialize (changing multi-core afterwards makes Eden
    // re-create the whole emulated system): /data/switch-emu/cpu-unsafe (faster, less exact floating
    // point) and /data/switch-emu/singlecore (multi-core is the default).
    // Set explicitly both ways: Eden's saved config (sdl2-config.ini) would otherwise keep values
    // from earlier runs (seen: multi-core stayed off after its default changed).
    {
        struct stat st;
        Settings::values.cpu_accuracy.SetValue(stat("/data/switch-emu/cpu-unsafe", &st) == 0
                                                   ? Settings::CpuAccuracy::Unsafe
                                                   : Settings::CpuAccuracy::Auto);
        Settings::values.use_multi_core.SetValue(stat("/data/switch-emu/singlecore", &st) != 0);
        Settings::values.cpuopt_unsafe_host_mmu.SetValue(true);
        // /data/switch-emu/async-shaders: draw without a pipeline until it's built in the background
        // (less stutter, brief glitches).
        Settings::values.use_asynchronous_shaders.SetValue(stat("/data/switch-emu/async-shaders", &st) == 0);
        // Performance overlay (launcher: Debug): /data/switch-emu/overlay.txt "<mode> <corner>".
        if (FILE* f = fopen("/data/switch-emu/overlay.txt", "r")) {
            int mode = 0, corner = 1;
            if (fscanf(f, "%d %d", &mode, &corner) == 2)
                ps4_overlay_configure(mode, corner);
            fclose(f);
        }
        // GPU: the mobile defaults (Android's) suit the PS4 - set explicitly, the saved config
        // would keep desktop values. /data/switch-emu/gpu-high: High accuracy for a game that needs it.
        Settings::values.gpu_accuracy.SetValue(stat("/data/switch-emu/gpu-high", &st) == 0 ? Settings::GpuAccuracy::High
                                                                                    : Settings::GpuAccuracy::Low);
        Settings::values.use_reactive_flushing.SetValue(false);
        Settings::values.max_anisotropy.SetValue(Settings::AnisotropyMode::Default);
        // Launcher: console mode (handheld renders at 720p) and resolution scale.
        Settings::values.use_docked_mode.SetValue(stat("/data/switch-emu/handheld", &st) == 0 ? Settings::ConsoleMode::Handheld
                                                                                       : Settings::ConsoleMode::Docked);
        Settings::values.resolution_setup.SetValue(stat("/data/switch-emu/res-050", &st) == 0   ? Settings::ResolutionSetup::Res1_2X
                                                   : stat("/data/switch-emu/res-075", &st) == 0 ? Settings::ResolutionSetup::Res3_4X
                                                                                          : Settings::ResolutionSetup::Res1X);
    }
    PS4_TRACE("main: cpu accuracy {}, multi-core {}, fastmem enabled {}, console mode {}, resolution setup {}",
              int(Settings::values.cpu_accuracy.GetValue()), Settings::values.use_multi_core.GetValue(),
              Settings::IsFastmemEnabled(), int(Settings::values.use_docked_mode.GetValue()),
              int(Settings::values.resolution_setup.GetValue()));
#endif
    PS4_TRACE("main: game path {}", filepath);
    PS4_STAGE("Initializing the emulated Switch");
    state->system.Initialize();
    PS4_TRACE("main: system initialized");

    InputCommon::InputSubsystem input_subsystem{};

    // Apply the command line arguments
    state->system.ApplySettings();
    Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Vulkan);
    // EDEN_NULL_RENDERER=1: run without graphics (PC baseline runs of the PS4 benchmark).
    if (getenv("EDEN_NULL_RENDERER"))
        Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Null);
#ifdef __OPENORBIS__
    // Test switch: create /data/switch-emu/null-renderer to run without graphics (CPU/memory bring-up).
    // Install runs keep the status screen (no renderer) to show progress.
    if (struct stat st; stat("/data/switch-emu/null-renderer", &st) == 0 || stat("/data/switch-emu/install-now", &st) == 0)
        Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Null);
    PS4_TRACE("main: renderer backend {}", int(Settings::values.renderer_backend.GetValue()));
    // A real renderer needs VideoOut; the status screen only runs without one.
    if (Settings::values.renderer_backend.GetValue() != Settings::RendererBackend::Null)
        ps4_status_stop();
#endif
    Common::Log::SetGlobalFilter(Common::Log::Filter(Common::Log::Level::Trace));

    switch (Settings::values.renderer_backend.GetValue()) {
#ifdef HAS_OPENGL
    case Settings::RendererBackend::OpenGL_GLSL:
    case Settings::RendererBackend::OpenGL_GLASM:
    case Settings::RendererBackend::OpenGL_SPIRV:
        state->emu_window = std::make_unique<EmuWindow_SDL3_GL>(&input_subsystem, state->system, fullscreen);
        break;
#endif
    case Settings::RendererBackend::Vulkan:
        state->emu_window = std::make_unique<EmuWindow_SDL3_VK>(&input_subsystem, state->system, fullscreen);
        break;
    case Settings::RendererBackend::Null:
        state->emu_window = std::make_unique<EmuWindow_SDL3_Null>(&input_subsystem, state->system, fullscreen);
        break;
    default:
        LOG_CRITICAL(Frontend, "Invalid renderer backend");
        return SDL_APP_FAILURE;
    }

#ifdef _WIN32
    Common::Windows::SetCurrentTimerResolutionToMaximum();
    state->system.CoreTiming().SetTimerResolutionNs(Common::Windows::GetCurrentTimerResolution());
#endif

    PS4_TRACE("main: window created");
    state->system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    state->system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    state->system.GetFileSystemController().CreateFactories(*state->system.GetFilesystem());
    state->system.GetUserChannel().clear();
#ifdef __OPENORBIS__
    if (struct stat st; stat("/data/switch-emu/install-now", &st) == 0) {
        remove("/data/switch-emu/install-now");
        PS4InstallPending(state->system);
    }
#endif

    Service::AM::FrontendAppletParameters load_parameters{
        .applet_id = Service::AM::AppletId::Application,
    };
    PS4_TRACE("main: loading game");
    PS4_STAGE("Loading {}", filepath);
    const Core::SystemResultStatus load_result = state->system.Load(*state->emu_window, filepath, load_parameters);
    PS4_TRACE("main: load result {}", int(load_result));
#ifdef __OPENORBIS__
    if (load_result == Core::SystemResultStatus::Success) {
        // The launcher's library: remember this game's real title and icon.
        PS4CacheTitle(state->system, filepath);
        // Saves live per user and title: show which user is active and whether this game has saved
        // before (so a missing save after a restart can be told apart from a changed user).
        const auto user = state->system.GetProfileManager().GetUser(Settings::values.current_user.GetValue());
        if (user) {
            const u128 id = user->AsU128();
            const std::string save = fmt::format("/data/switch-emu/nand/user/save/0000000000000000/{:016X}{:016X}/{:016X}",
                                                 id[1], id[0], state->system.GetApplicationProcessProgramID());
            struct stat st;
            PS4_TRACE("saves: user {}, {} at {}", user->FormattedString(),
                      stat(save.c_str(), &st) == 0 ? "existing save" : "no save yet", save);
        } else {
            PS4_TRACE("saves: no user profile {}", Settings::values.current_user.GetValue());
        }
    }
#endif
    if (load_result != Core::SystemResultStatus::Success) {
        PS4_STAGE("Loading failed (error {})", int(load_result));
#ifdef __OPENORBIS__
        // Back to the menu (or on with the benchmark suite) instead of shutting down a system
        // that never started - that crashed in InputSubsystem::Shutdown on hardware.
        ps4_menu_load_failed(int(load_result), filepath.c_str());
#endif
    }
    switch (load_result) {
    case Core::SystemResultStatus::Success:
        break; // Expected case
    case Core::SystemResultStatus::ErrorGetLoader:
        LOG_CRITICAL(Frontend, "Failed to obtain loader for {}!", filepath);
        return SDL_APP_FAILURE;
    case Core::SystemResultStatus::ErrorLoader:
        LOG_CRITICAL(Frontend, "Failed to load ROM!");
        return SDL_APP_FAILURE;
    case Core::SystemResultStatus::ErrorNotInitialized:
        LOG_CRITICAL(Frontend, "CPUCore not initialized");
        return SDL_APP_FAILURE;
    case Core::SystemResultStatus::ErrorVideoCore:
        LOG_CRITICAL(Frontend, "Failed to initialize VideoCore!");
        return SDL_APP_FAILURE;
    default:
        const u16 loader_id = u16(Core::SystemResultStatus::ErrorLoader);
        const u16 error_id = u16(load_result) - loader_id;
        LOG_CRITICAL(Frontend,
            "While attempting to load the ROM requested, an error occurred. Check that "
            "the keys and firmware are installed (see the README).\n\nError Code: "
            "{:04X}-{:04X}\nError Description: {}",
            loader_id, error_id, Loader::ResultStatus(error_id));
        return SDL_APP_FAILURE;
    }

    if (use_multiplayer) {
        if (auto member = Network::GetRoomMember().lock()) {
            member->BindOnChatMessageReceived(OnMessageReceived);
            member->BindOnStatusMessageReceived(OnStatusMessageReceived);
            member->BindOnStateChanged(OnStateChanged);
            member->BindOnError(OnNetworkError);
            LOG_DEBUG(Network, "Start connection to {}:{} with nickname {}", address, port, nickname);
            member->Join(nickname, address.c_str(), port, 0, Network::NoPreferredIP, password);
        } else {
            LOG_ERROR(Network, "Could not access RoomMember");
            return SDL_APP_FAILURE;
        }
    }

    {
        auto emulated_controller = state->system.HIDCore().GetEmulatedController(Core::HID::NpadIdType::Player1);
        if (const char* script = getenv("EDEN_AUTOPRESS")) {
            StartAutopress(input_subsystem, emulated_controller, script);
            goto skip_controller_mapping;
        }
        auto const input_devices = input_subsystem.GetInputDevices();
        for (const auto& e : input_devices) {
            LOG_INFO(Input, "FOUND=Device -> {} {}", e.Get("display", "Unknown"), e.Serialize());
        }
        // First SDL gamepad (the DualShock 4 on the PS4); was a fixed input_devices[3], which is
        // out of range without a controller (crash on a headless PC).
        auto const it = std::find_if(input_devices.begin(), input_devices.end(),
                                     [](const auto& e) { return e.Get("engine", "") == "sdl"; });
        if (it == input_devices.end()) {
            LOG_WARNING(Input, "No SDL gamepad found; controller not auto-mapped");
            goto skip_controller_mapping;
        }
        auto const& device = *it;
        LOG_INFO(Input, "selected? {}", device.Serialize());
        auto const button_mappings = input_subsystem.GetButtonMappingForDevice(device);
        auto const analog_mappings = input_subsystem.GetAnalogMappingForDevice(device);
        auto const motion_mappings = input_subsystem.GetMotionMappingForDevice(device);
        for (const auto& pair : button_mappings)
            emulated_controller->SetButtonParam(pair.first, pair.second);
        for (const auto& pair : analog_mappings)
            emulated_controller->SetStickParam(pair.first, pair.second);
        for (const auto& pair : motion_mappings)
            emulated_controller->SetMotionParam(pair.first, pair.second);

        for (const auto& pair : button_mappings)
            LOG_INFO(Input, "btn:{}:{}", pair.first, pair.second.Serialize());
        for (const auto& pair : analog_mappings)
            LOG_INFO(Input, "ang:{}:{}", pair.first, pair.second.Serialize());
        for (const auto& pair : motion_mappings)
            LOG_INFO(Input, "mot:{}:{}", pair.first, pair.second.Serialize());

        emulated_controller->SaveCurrentConfig();
        emulated_controller->ReloadFromSettings();

        auto const mapped_devices = emulated_controller->GetMappedDevices();
        LOG_INFO(Input, "AND SO, WHAT MAPPED DEVICES WE GOT?");
        for (const auto& e : mapped_devices) {
            LOG_INFO(Input, "Mapped=Device? -> {}", e.Get("display", "Unknown"));
        }
    }
skip_controller_mapping:

    // Core is loaded, start the GPU (makes the GPU contexts current to this thread)
    PS4_TRACE("main: starting GPU");
    state->system.GPU().Start();
    state->system.GetCpuManager().OnGpuReady();
    PS4_TRACE("main: GPU started");

    if (Settings::values.use_disk_shader_cache.GetValue()) {
        const u64 load_start = SDL_GetTicks();
        static size_t s_pipelines = 0;
        state->system.Renderer().ReadRasterizer()->LoadDiskResources(
            state->system.GetApplicationProcessProgramID(), std::stop_token{},
            [](VideoCore::LoadCallbackStage stage, size_t value, size_t total) {
                if (stage == VideoCore::LoadCallbackStage::Build)
                    s_pipelines = total;
#ifdef __OPENORBIS__
                // Progress on the loading screen, a few times a second.
                static u64 s_last = 0;
                if (const u64 now = SDL_GetTicks(); stage == VideoCore::LoadCallbackStage::Build && now - s_last >= 200) {
                    s_last = now;
                    ps4_swapchain_show_loading(
                        fmt::format("Building shaders from the cache: {} of {}", value, total).c_str());
                }
#endif
            });
        PS4_TRACE("shader cache: {} pipelines built from disk in {:.1f} s (async shaders {})", s_pipelines,
                  (SDL_GetTicks() - load_start) / 1000.0, Settings::values.use_asynchronous_shaders.GetValue());
    }

    // don't do anything, SDL3 already exists for us :D
    state->system.RegisterExitCallback([state] {
        state->system.DeviceMemory().buffer.LogFastmemStats("guest exit");
        PS4_TRACE("main: guest program exited");
        PS4_STAGE("Finished - the program exited. Close the app with the PS button.");
#ifdef __OPENORBIS__
        ps4_menu_guest_exited();
#endif
    });

    // QLaunch launched applications (should) now reload shader cache.
    static std::mutex shader_cache_reload_mutex;
    state->system.RegisterApplicationChangedCallback([&state](u64 changed_program_id) {
        if (!Settings::values.use_disk_shader_cache.GetValue()) {
            return;
        }

        std::scoped_lock lk{shader_cache_reload_mutex};

        LOG_INFO(Frontend, "Reloading disk shader cache for {:016X}", changed_program_id);

        state->system.Pause();
        state->system.GPU().WaitForIdle();
        state->system.GPU().ObtainContext();

        state->system.Renderer().ReadRasterizer()->LoadDiskResources(
            changed_program_id, std::stop_token{},
            [](VideoCore::LoadCallbackStage, size_t, size_t) {});

        state->system.GPU().ReleaseContext();
        state->system.Run();
    });

    PS4_TRACE("main: running emulation");
#ifdef __OPENORBIS__
    if (struct stat st; stat("/data/switch-emu/profile", &st) == 0)
        ps4_sampler_start();
#endif
    state->system.DeviceMemory().buffer.LogFastmemStats("emulation start");
    PS4_STAGE("Running {} (null renderer: no picture, output on the right)", filepath);
    void(state->system.Run());
    if (state->system.DebuggerEnabled())
        state->system.InitializeDebugger();
    // SDL3 main callbacks: SDL_APP_SUCCESS here would mean "quit now"; keep iterating.
    return SDL_APP_CONTINUE;
}
#ifdef __OPENORBIS__
#include "ps4_launcher.h"
static std::string g_ps4_last_perf = "Frame rate: measuring...";

// First connected gamepad's buttons as a bit set (SDL_GamepadButton bits).
static u32 PS4GamepadButtons() {
    u32 bits = 0;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        if (count > 0)
            if (SDL_Gamepad* gp = SDL_GetGamepadFromID(ids[0]))
                for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 32; b++)
                    if (SDL_GetGamepadButton(gp, SDL_GamepadButton(b)))
                        bits |= 1u << b;
        SDL_free(ids);
    }
    return bits;
}

// In-game Home menu (hold L3 + R3): pauses the game and draws over its last frame.
static void PS4HomeMenu(SdlState* state) {
    PS4_TRACE("main: Home menu");
    void(state->system.Pause());
    // Let the GPU thread finish what the game queued (its presents would flip over the menu).
    state->system.GPU().WaitForIdle();
    if (!ps4_swapchain_overlay_begin()) { // no frame shown yet: just leave
        ps4_menu_restart_now();
        return;
    }
    static const char* const kOverlayLabels[] = {"Performance overlay: Off", "Performance overlay: FPS",
                                                  "Performance overlay: Detailed"};
    const char* items[] = {"Resume", "Restart game", "Quit to launcher", kOverlayLabels[ps4_overlay_mode() % 3]};
    Ps4HomeMenu menu{ps4_launcher_loading_title(), g_ps4_last_perf.c_str(), items, 4, 0};
    struct Ctx {
        const Ps4HomeMenu* menu;
        bool capture;
    } ctx{&menu, true};
    const auto draw = [](uint32_t* dst, const uint32_t* src, int pitch, void* c) {
        auto* x = static_cast<Ctx*>(c);
        ps4_status_draw_home(dst, src, pitch, x->menu, x->capture);
    };
    ps4_swapchain_overlay_draw(draw, &ctx);
    ctx.capture = false;
    constexpr u32 kUp = 1u << SDL_GAMEPAD_BUTTON_DPAD_UP, kDown = 1u << SDL_GAMEPAD_BUTTON_DPAD_DOWN,
                  kSelect = 1u << SDL_GAMEPAD_BUTTON_SOUTH, kBack = 1u << SDL_GAMEPAD_BUTTON_EAST,
                  kSticks = (1u << SDL_GAMEPAD_BUTTON_LEFT_STICK) | (1u << SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    u32 previous = PS4GamepadButtons() | kSticks; // ignore what's still held from opening it
    for (;;) {
        SDL_PumpEvents();
        SDL_UpdateGamepads();
        const u32 now = PS4GamepadButtons();
        const u32 pressed = now & ~previous;
        previous = now;
        const int old = menu.selected;
        if (pressed & kUp)
            menu.selected = (menu.selected + menu.count - 1) % menu.count;
        if (pressed & kDown)
            menu.selected = (menu.selected + 1) % menu.count;
        if (menu.selected != old || ps4_status_home_animating()) // the panel rising in, the highlight gliding
            ps4_swapchain_overlay_draw(draw, &ctx);
        if ((pressed & kBack) || ((pressed & kSelect) && menu.selected == 0)) {
            ps4_swapchain_overlay_end();
            state->system.Run();
            PS4_TRACE("main: Home menu closed, resuming");
            return;
        }
        if ((pressed & kSelect) && menu.selected == 1) {
            if (FILE* f = fopen("/data/switch-emu/autostart", "w"))
                fclose(f);
            ps4_menu_restart_now();
        }
        if ((pressed & kSelect) && menu.selected == 2)
            ps4_menu_restart_now();
        if ((pressed & kSelect) && menu.selected == 3) { // cycle the overlay (this session)
            const int mode = (ps4_overlay_mode() + 1) % 3;
            int corner = 1;
            if (FILE* f = fopen("/data/switch-emu/overlay.txt", "r")) {
                int ignored = 0;
                if (fscanf(f, "%d %d", &ignored, &corner) != 2)
                    corner = 1;
                fclose(f);
            }
            ps4_overlay_configure(mode, corner);
            items[3] = kOverlayLabels[mode];
            ps4_swapchain_overlay_draw(draw, &ctx);
        }
        SDL_Delay(16);
    }
}
#endif

// EDEN_SCREENSHOTS=<dir>: saves the game's picture there every EDEN_SCREENSHOT_EVERY seconds
// (default 5) as shot-<seconds>.ppm, to see where a game is when nobody watches the screen (PC
// runs driven by EDEN_AUTOPRESS).
static void MaybeScreenshot(Core::System& system) {
    static const char* const dir = std::getenv("EDEN_SCREENSHOTS");
    if (!dir || !system.IsPoweredOn() || system.Renderer().IsScreenshotPending())
        return;
    static const double every = std::getenv("EDEN_SCREENSHOT_EVERY") ? std::atof(std::getenv("EDEN_SCREENSHOT_EVERY")) : 5.0;
    static const auto start = std::chrono::steady_clock::now();
    static double next = every;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (now < next)
        return;
    next = now + every;
    constexpr u32 kW = 1280, kH = 720;
    static std::vector<u8> pixels(size_t(kW) * kH * 4);
    const int seconds = int(now);
    system.Renderer().RequestScreenshot(
        pixels.data(),
        [seconds](bool invert_y) {
            const std::string path = fmt::format("{}/shot-{:04d}.ppm", dir, seconds);
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fprintf(f, "P6 %u %u 255\n", kW, kH);
                std::vector<u8> row(size_t(kW) * 3);
                for (u32 y = 0; y < kH; y++) {
                    const u8* src = pixels.data() + size_t(invert_y ? kH - 1 - y : y) * kW * 4;
                    for (u32 x = 0; x < kW; x++) { // BGRA -> RGB
                        row[x * 3] = src[x * 4 + 2];
                        row[x * 3 + 1] = src[x * 4 + 1];
                        row[x * 3 + 2] = src[x * 4];
                    }
                    std::fwrite(row.data(), 1, row.size(), f);
                }
                std::fclose(f);
            }
        },
        Layout::DefaultFrameLayout(kW, kH));
}

extern "C" SDL_AppResult SDL_AppIterate(void *appstate) {
    SdlState *state = (SdlState *)appstate;
    MaybeScreenshot(state->system);
#ifdef __OPENORBIS__
    // Nothing to draw here (rendering happens on its own thread); don't spin a whole Jaguar core.
    SDL_Delay(4);
    ps4_menu_poll_restart();
    // Hold L3 + R3 for 1 s: the Home menu (the PS button belongs to the system).
    {
        static u64 s_combo_since = 0;
        bool held = false;
        int count = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
            for (int i = 0; i < count && !held; i++)
                if (SDL_Gamepad* gp = SDL_GetGamepadFromID(ids[i]))
                    held = SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_LEFT_STICK) &&
                           SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
            SDL_free(ids);
        }
        const u64 now = SDL_GetTicks();
        if (!held) {
            s_combo_since = 0;
        } else if (s_combo_since == 0) {
            s_combo_since = now;
        } else if (now - s_combo_since >= 1000 && state->system.IsPoweredOn()) {
            s_combo_since = 0;
            PS4HomeMenu(state);
        }
    }
    // Performance every 5 s in boot-trace.log (the status screen is hidden under Vulkan).
    static u64 s_last_perf = SDL_GetTicks();
    if (const u64 now = SDL_GetTicks(); now - s_last_perf >= 5000 && state->system.IsPoweredOn()) {
        s_last_perf = now;
        const Core::PerfStatsResults perf = state->system.GetAndResetPerfStats();
        static unsigned long long s_gpu_wait = 0;
        const unsigned long long gpu_wait = ps4_gpu_wait_us();
        PS4_TRACE("perf: game {:.1f} fps, emulation speed {:.0f}%, {:.1f} ms per frame, waiting for the GPU {:.0f}% of "
                  "the time",
                  perf.average_game_fps, perf.emulation_speed * 100.0, perf.frametime * 1000.0,
                  double(gpu_wait - s_gpu_wait) / 50000.0); // us per 5 s -> %
        s_gpu_wait = gpu_wait;
        g_ps4_last_perf = fmt::format("Frame rate: {:.0f} fps (last 5 s)", perf.average_game_fps);
    }
#endif
    return state->emu_window->IsOpen() ? SDL_APP_CONTINUE : SDL_APP_SUCCESS;
}
extern "C" SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    SdlState *state = (SdlState *)appstate;
    state->emu_window->OnEvent(*event);
    // Only stop once the window has been closed (SDL_APP_SUCCESS ends the app).
    return state->emu_window->IsOpen() ? SDL_APP_CONTINUE : SDL_APP_SUCCESS;
}
extern "C" void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    SdlState *state = (SdlState *)appstate;
    PS4_TRACE("quit: result {}", int(result));
    if (!state)
        return; // SDL_AppInit failed before creating the state
    state->system.DetachDebugger();
    void(state->system.Pause());
    state->system.ShutdownMainProcess();
    delete state;
}

#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"
