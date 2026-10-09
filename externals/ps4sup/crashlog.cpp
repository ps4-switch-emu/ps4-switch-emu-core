// SPDX-FileCopyrightText: Copyright 2026 DolphinPS4 (port/ps4_crashlog.cpp), adapted for Eden
// SPDX-License-Identifier: GPL-2.0-or-later

// Crash diagnostics for PS4 test builds.
//
// PS4 coredumps are encrypted and klog is shared with other tools, so the app reports its own
// crashes: a constructor that runs before all others (priority 101) starts
// /data/switch-emu/logs/boot-trace.log and installs a handler for fatal signals that appends the
// signal, fault address, registers and the code addresses found on the stack to
// /data/switch-emu/logs/crash.log. Addresses are printed relative to the start of .text, i.e. as ELF
// virtual addresses: symbolize with `llvm-symbolizer --obj=<elf> 0x...`.
//
// Handlers installed later (dynarmic's fastmem handler, once enabled) chain to this one for faults
// they don't handle.

#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cxxabi.h>
#include <exception>
#include <typeinfo>

extern "C" char __text_start[];
extern "C" const char* ps4_sampler_thread_name(); // sampler.cpp  // defined by the OpenOrbis link.x at the start of .text
extern "C" int sceKernelInternalMemoryGetAvailableSize(size_t* size);
extern "C" uint32_t sceSysmoduleLoadModuleInternal(uint32_t id);
// externals/ps4sup/status_screen.cpp (weak: optional)
extern "C" __attribute__((weak)) void ps4_status_push(const char* line);
extern "C" __attribute__((weak)) void ps4_status_crashed();
extern "C" int32_t sceSysmoduleLoadModule(uint16_t id);
extern "C" void ps4_boot_trace(const char* stage);
extern "C" int32_t sceKernelAvailableFlexibleMemorySize(size_t* size);
extern "C" size_t sceKernelGetDirectMemorySize();
extern "C" int32_t sceKernelAvailableDirectMemorySize(off_t start, off_t end, size_t align,
                                                     off_t* start_out, size_t* size_out);
extern "C" int32_t sceKernelQueryMemoryProtection(void* address, void** start, void** end,
                                                  uint32_t* protection);

namespace {

// FreeBSD values (the PS4 kernel's); the OpenOrbis musl headers may carry Linux ones.
constexpr int kSigIll = 4, kSigAbrt = 6, kSigFpe = 8, kSigBus = 10, kSigSegv = 11;
// Eden: also the signals a bad system call or trap raises (SIGSYS: e.g. a Linux syscall number
// from musl reaching the FreeBSD kernel).
constexpr int kSigTrap = 5, kSigEmt = 7, kSigSys = 12;
constexpr int kSaSiginfo = 0x40;
constexpr int kSaOnstack = 0x1;
constexpr int kMcontextOffset = 0x40;  // measured on hardware
// Registers in FreeBSD mcontext order (after mc_onstack).
constexpr const char* kRegNames[] = {"rdi", "rsi", "rdx", "rcx", "r8",  "r9",  "rax", "rbx",
                                     "rbp", "r10", "r11", "r12", "r13", "r14", "r15"};
constexpr int kRipIndex = 20, kRspIndex = 23;
constexpr uintptr_t kTextWindow = 0x4000000;  // 64 MiB covers the eboot's code

int g_traceFd = -1;

const char kGuide[] = "PS4 Switch Emu\n"
                      "\n"
                      "games/       your games (.nsp, .xci) and homebrew (.nro)\n"
                      "keys/        prod.keys and title.keys, dumped from your own Switch\n"
                      "nand/system/Contents/registered/\n"
                      "             the Switch firmware: its .nca files, loose\n"
                      "install/     updates and DLC (.nsp) to install from the launcher\n"
                      "logs/        logs and crash reports - attach these to bug reports\n"
                      "save-backups/  copies made by Settings > Tools > Back up saves\n"
                      "\n"
                      "Game saves are in nand/user/save/.\n";

// Everything the app and the user need, made on the first start (and again if deleted), with a
// guide to what goes where.
void createFolders() {
    static const char* const kFolders[] = {
        "/data/switch-emu",          "/data/switch-emu/logs",       "/data/switch-emu/games",
        "/data/switch-emu/keys",     "/data/switch-emu/install",    "/data/switch-emu/titles",
        "/data/switch-emu/save-backups", "/data/switch-emu/sdmc",   "/data/switch-emu/nand",
        "/data/switch-emu/nand/system", "/data/switch-emu/nand/system/Contents",
        "/data/switch-emu/nand/system/Contents/registered",
    };
    for (const char* folder : kFolders)
        mkdir(folder, 0777);
    struct stat st;
    if (stat("/data/switch-emu/README.txt", &st) != 0) {
        const int fd = open("/data/switch-emu/README.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) {
            write(fd, kGuide, sizeof(kGuide) - 1);
            close(fd);
        }
    }
}

// Opened on first use: the heap may log before this file's constructor runs.
int traceFd() {
    if (g_traceFd < 0) {
        createFolders();
        // Keep the last run's trace (after a crash, the next start would overwrite it).
        rename("/data/switch-emu/logs/boot-trace.log", "/data/switch-emu/logs/boot-trace.prev.log");
        g_traceFd = open("/data/switch-emu/logs/boot-trace.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    }
    return g_traceFd;
}

void writeAll(int fd, const char* text) {
    size_t length = strlen(text);
    while (length > 0) {
        ssize_t written = write(fd, text, length);
        if (written <= 0)
            return;
        text += written;
        length -= static_cast<size_t>(written);
    }
}

__attribute__((format(printf, 2, 3))) void writeFormat(int fd, const char* fmt, ...) {
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    writeAll(fd, line);
}

bool isCode(uint64_t address) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(__text_start);
    return address >= base && address < base + kTextWindow;
}

uint64_t toElf(uint64_t address) { return address - reinterpret_cast<uintptr_t>(__text_start); }

void setHandler(int sig, void (*handler)(int, siginfo_t*, void*), int flags) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    // The handler union is the first member; its member names differ between headers.
    memcpy(&action, &handler, sizeof(handler));
    action.sa_flags = flags;
    sigemptyset(&action.sa_mask);
    sigaction(sig, &action, nullptr);
}

void crashHandler(int sig, siginfo_t* info, void* context) {
    static volatile int s_depth = 0;
    if (s_depth++ > 0) {
        // Crashed while reporting: give up. Returning with the handler still installed would
        // re-run the faulting instruction forever, so fall back to the default action first.
        setHandler(sig, nullptr, 0);
        return;
    }

    const int fd = open("/data/switch-emu/logs/crash.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        const auto* regs = reinterpret_cast<const uint64_t*>(static_cast<char*>(context) + kMcontextOffset);
        const uint64_t rip = regs[kRipIndex];
        const uint64_t rsp = regs[kRspIndex];
        const uint64_t address = *reinterpret_cast<const uint64_t*>(reinterpret_cast<char*>(info) + 24);
        const char* thread = ps4_sampler_thread_name();
        writeFormat(fd, "\n=== signal %d (code %d), fault address 0x%llx, thread %s\n", sig,
                    *reinterpret_cast<const int*>(reinterpret_cast<char*>(info) + 8),
                    static_cast<unsigned long long>(address), thread ? thread : "main (or unnamed)");
        writeFormat(fd, "rip 0x%llx%s elf 0x%llx, rsp 0x%llx, text at %p\n",
                    static_cast<unsigned long long>(rip), isCode(rip) ? "" : " (not eboot code)",
                    static_cast<unsigned long long>(toElf(rip)), static_cast<unsigned long long>(rsp),
                    static_cast<void*>(__text_start));
        for (int i = 0; i < 15; i++)
            writeFormat(fd, "%s 0x%llx%s", kRegNames[i], static_cast<unsigned long long>(regs[1 + i]),
                        i % 4 == 3 ? "\n" : "  ");
        writeAll(fd, "\ncode addresses on the stack (elf):\n");
        const auto* stack = reinterpret_cast<const uint64_t*>(rsp);
        // Only as far as the stack's mapping goes: a thread near the top of its stack has
        // nothing mapped 16 KiB above rsp.
        int words = 2048;
        void* map_start = nullptr;
        void* map_end = nullptr;
        uint32_t prot = 0;
        if (sceKernelQueryMemoryProtection(reinterpret_cast<void*>(rsp), &map_start, &map_end, &prot) == 0) {
            const uint64_t avail = (reinterpret_cast<uint64_t>(map_end) - rsp) / 8;
            if (avail < uint64_t(words))
                words = int(avail);
        }
        int found = 0;
        for (int i = 0; i < words && found < 48; i++) {
            if (isCode(stack[i])) {
                writeFormat(fd, "  [rsp+0x%x] 0x%llx\n", i * 8, static_cast<unsigned long long>(toElf(stack[i])));
                found++;
            }
        }
        close(fd);
    }
    if (traceFd() >= 0)
        writeAll(traceFd(), "crashed (see crash.log)\n");
    if (ps4_status_crashed) {
        ps4_status_crashed();
        usleep(800 * 1000); // let the status screen show it before the system's error dialog
    }

    // Back to the default action: returning re-executes the faulting instruction, which now
    // ends the process with the system's crash report.
    setHandler(sig, nullptr, 0);
    s_depth = 0;
}

// std::terminate (e.g. an uncaught exception): record what was thrown, then abort as usual.
[[noreturn]] void terminateHandler() {
    char message[512] = "std::terminate called";
    if (const std::type_info* type = abi::__cxa_current_exception_type()) {
        int status = 0;
        char* name = abi::__cxa_demangle(type->name(), nullptr, nullptr, &status);
        snprintf(message, sizeof(message), "uncaught exception %s", name ? name : type->name());
        // e.g. vk::Exception's what() names the VkResult.
        try {
            throw;
        } catch (const std::exception& e) {
            const size_t len = strlen(message);
            snprintf(message + len, sizeof(message) - len, ": %s", e.what());
        } catch (...) {
        }
    }
    const int fd = open("/data/switch-emu/logs/crash.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        writeFormat(fd, "\n=== %s\n", message);
        close(fd);
    }
    if (traceFd() >= 0)
        writeFormat(traceFd(), "%s\n", message);
    abort();
}

// Calling into a system library whose module isn't loaded kills the app with
// PRX_NOT_RESOLVED_FUNCTION - no signal, no crash.log (DolphinPS4 PlatformPS4.cpp, love-ps4).
// Linking against a library's stub is not enough: load each module Eden and SDL call into before
// any of their code runs (Eden).
void loadSystemModules() {
    struct Module {
        const char* name;
        uint32_t id;
        bool internal;
    };
    static constexpr Module modules[] = {
        {"SystemService", 0x80000010, true}, // ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE
        {"UserService", 0x80000011, true},   // ORBIS_SYSMODULE_INTERNAL_USER_SERVICE
        {"VideoOut", 0x80000022, true},      // ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT
        {"AudioOut", 0x80000001, true},      // ORBIS_SYSMODULE_INTERNAL_AUDIOOUT
        {"Pad", 0x80000024, true},           // ORBIS_SYSMODULE_INTERNAL_PAD
        {"Net", 0x8000001C, true},           // ORBIS_SYSMODULE_INTERNAL_NET
        {"Random", 0x00BA, false},           // ORBIS_SYSMODULE_RANDOM (getentropy, ps4sup/stub.cpp)
    };
    for (const Module& m : modules) {
        const int result = m.internal ? static_cast<int>(sceSysmoduleLoadModuleInternal(m.id))
                                      : sceSysmoduleLoadModule(static_cast<uint16_t>(m.id));
        char line[96];
        snprintf(line, sizeof(line), "system module %s: %#x", m.name, static_cast<unsigned>(result));
        ps4_boot_trace(line);
    }
}

// The handler runs on its own stack, so a stack overflow is still reported (Eden).
alignas(16) char g_altStack[256 * 1024];

__attribute__((constructor(101))) void earlyInit() {
    stack_t ss{};
    ss.ss_sp = g_altStack;
    ss.ss_size = sizeof(g_altStack);
    ss.ss_flags = 0;
    const int alt = sigaltstack(&ss, nullptr);
    if (traceFd() >= 0)
        writeFormat(traceFd(), "crash handler alt stack: %d\n", alt);
    if (traceFd() >= 0)
        writeFormat(traceFd(), "static init (text at %p)\n", static_cast<void*>(__text_start));
    std::set_terminate(terminateHandler);
    const int signals[] = {kSigIll, kSigAbrt, kSigFpe, kSigBus, kSigSegv, kSigTrap, kSigEmt, kSigSys};
    for (int sig : signals)
        setHandler(sig, crashHandler, kSaSiginfo | kSaOnstack);

    loadSystemModules();
}

}  // namespace

// Appends "<what>: callers 0x.. 0x.." to the boot trace: code addresses found on the calling
// thread's stack (ELF addresses, symbolize like crash.log). A cheap "who calls this" without
// frame pointers; may include stale return addresses.
extern "C" void ps4_boot_trace(const char* stage);
extern "C" __attribute__((noinline)) void ps4_trace_callers(const char* what) {
    // Read from the stack pointer itself: walking past a local variable is undefined behaviour
    // the optimizer is free to drop (an earlier version printed nothing).
    const uint64_t* stack;
    asm volatile("mov %%rsp, %0" : "=r"(stack));
    char line[320];
    int len = snprintf(line, sizeof(line), "%s: callers", what);
    int found = 0;
    for (int i = 0; i < 512 && found < 10 && len < 280; i++) {
        if (isCode(stack[i])) {
            len += snprintf(line + len, sizeof(line) - len, " 0x%llx",
                            static_cast<unsigned long long>(toElf(stack[i])));
            found++;
        }
    }
    ps4_boot_trace(line);
}

// Appends a line to /data/switch-emu/logs/boot-trace.log (startup milestones), prefixed with the
// seconds since the first trace line.
extern "C" void ps4_boot_trace(const char* stage) {
    const int fd = traceFd();
    if (fd < 0)
        return;
    static timespec s_start;
    static bool s_started = false;
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!s_started) {
        s_start = now;
        s_started = true;
    }
    const double seconds = static_cast<double>(now.tv_sec - s_start.tv_sec) +
                           static_cast<double>(now.tv_nsec - s_start.tv_nsec) / 1e9;
    // One write per line, so lines from different threads don't interleave.
    // Room for the monitor's long lines; a cut line still ends with its newline (a truncated
    // monitor line used to run into the next one).
    char line[2048];
    const size_t length = strlen(stage);
    const bool newline = length == 0 || stage[length - 1] != '\n';
    // Free kernel-internal memory (pthread objects etc.; exhausting it kills the app without a
    // signal) and flexible memory, on every line (Eden).
    size_t internal_free = 0, flexible_free = 0;
    sceKernelInternalMemoryGetAvailableSize(&internal_free);
    sceKernelAvailableFlexibleMemorySize(&flexible_free);
    // Largest free block of direct memory: what's left for the GPU (RADV) after emulated DRAM.
    off_t dmem_at = 0;
    size_t dmem_free = 0;
    sceKernelAvailableDirectMemorySize(0, off_t(sceKernelGetDirectMemorySize()), 16 * 1024, &dmem_at,
                                       &dmem_free);
    const int written =
        snprintf(line, sizeof(line), "[%8.3f] [int %zu KiB, flex %zu MiB, dmem %zu MiB] %s%s",
                 seconds, internal_free >> 10, flexible_free >> 20, dmem_free >> 20, stage,
                 newline ? "\n" : "");
    if (written >= static_cast<int>(sizeof(line)))
        line[sizeof(line) - 2] = '\n';
    writeAll(fd, line);
    if (ps4_status_push) {
        char shown[200];
        snprintf(shown, sizeof(shown), "[%6.1f] %s", seconds, stage);
        ps4_status_push(shown);
    }
}
