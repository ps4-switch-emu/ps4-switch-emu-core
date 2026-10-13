// SPDX-FileCopyrightText: Copyright 2026 DolphinPS4 (port/ps4_sampler.cpp), adapted for Eden
// SPDX-License-Identifier: GPL-2.0-or-later

// Sampling profiler for PS4 test builds.
//
// PS4 has no perf/Razor access for homebrew, so the app samples itself: a helper thread sends
// SIGPROF to each registered thread every millisecond, and the handler records the interrupted
// instruction pointer. Every 10 seconds the samples are aggregated and appended to
// /data/switch-emu/logs/samples.log: per thread, how much time went to JIT code (the recompiled guest),
// eboot code and system modules, then the busiest eboot addresses (ELF virtual addresses,
// symbolize with `llvm-symbolizer --obj=<elf> 0x...` against that version's eden-cli) and JIT
// addresses (16-byte buckets).
//
// Threads register themselves through Common::SetCurrentThreadName (every named Eden thread);
// sampling starts with ps4_sampler_start() (the start menu's Profiler option).

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>

extern "C" char __text_start[]; // defined by the OpenOrbis link.x at the start of .text
extern "C" void ps4_boot_trace(const char* stage);
extern "C" void ps4_sampler_register_thread(const char* name);

namespace {

constexpr int kSigProf = 27; // FreeBSD value
constexpr int kSaSiginfo = 0x40, kSaRestart = 0x0002;
constexpr int kMcontextOffset = 0x40; // measured on hardware (see crashlog.cpp)
constexpr int kRipIndex = 20;
constexpr uint64_t kSystemLibs = 0x800000000ULL, kSysTag = 1ULL << 62, kJitTag = 1ULL << 63;
constexpr uint64_t kCallerTag = 1ULL << 61; // a system-library sample, recorded as its eboot caller
constexpr uintptr_t kTextWindow = 0x4000000;
constexpr int kMaxThreads = 32;
constexpr int kMaxSamples = 16384; // per thread and window (10 s at 1 kHz fits)

struct ThreadSamples {
    std::atomic<pthread_t> thread{};
    char name[32]{};
    uint64_t samples[kMaxSamples];
    std::atomic<int> count{0};
    // The thread's own CPU clock, read in the handler (only the thread itself can): the CPU time
    // actually used per window, which tells working from waiting (both show up as "sys").
    std::atomic<uint64_t> cpu_ns{0};
    uint64_t cpu_ns_reported = 0;
    // An address on the thread's stack when it registered: everything up to it is mapped, so the
    // handler's stack scan stays below it.
    std::atomic<uintptr_t> stack_hint{0};
};
constexpr clockid_t kThreadCpuClock = clockid_t(14); // FreeBSD CLOCK_THREAD_CPUTIME_ID

ThreadSamples g_threads[kMaxThreads];
std::atomic<int> g_thread_count{0};
std::atomic<bool> g_on{false};

void profHandler(int, siginfo_t*, void* context) {
    const pthread_t self = pthread_self();
    const int threads = g_thread_count.load(std::memory_order_acquire);
    for (int i = 0; i < threads; i++) {
        ThreadSamples& t = g_threads[i];
        if (t.thread.load(std::memory_order_relaxed) != self)
            continue;
        timespec cpu;
        if (clock_gettime(kThreadCpuClock, &cpu) == 0)
            t.cpu_ns.store(uint64_t(cpu.tv_sec) * 1000000000ull + uint64_t(cpu.tv_nsec), std::memory_order_relaxed);
        const auto* regs = reinterpret_cast<const uint64_t*>(static_cast<char*>(context) + kMcontextOffset);
        const int n = t.count.load(std::memory_order_relaxed);
        if (n < kMaxSamples) {
            uint64_t sample = regs[kRipIndex];
            if (sample >= kSystemLibs && sample < 2 * kSystemLibs) {
                // In a system library (a syscall, a lock...): record who called it - the first eboot
                // address in the top of the stack - so the report can say what the time was for.
                const uintptr_t text = reinterpret_cast<uintptr_t>(__text_start);
                const uint64_t rsp = regs[kRipIndex + 3];
                const auto* stack = reinterpret_cast<const uint64_t*>(rsp);
                const uintptr_t limit = t.stack_hint.load(std::memory_order_relaxed);
                const int words = rsp + 48 * 8 <= limit ? 48 : 0;
                for (int w = 0; w < words; w++)
                    if (stack[w] >= text && stack[w] < text + kTextWindow) {
                        sample = stack[w] | kCallerTag;
                        break;
                    }
            }
            t.samples[n] = sample;
            t.count.store(n + 1, std::memory_order_release);
        }
        return;
    }
}

void writeLine(int fd, const char* text) {
    size_t length = strlen(text);
    while (length > 0) {
        const ssize_t written = write(fd, text, length);
        if (written <= 0)
            return;
        text += written;
        length -= size_t(written);
    }
}

// Aggregates one thread's window: classifies, sorts and prints the busiest entries.
void report(int fd, ThreadSamples& t, double window_start, double window_seconds) {
    static uint64_t s[kMaxSamples];
    const int n = std::min(t.count.load(std::memory_order_acquire), kMaxSamples);
    memcpy(s, t.samples, sizeof(uint64_t) * size_t(n));
    t.count.store(0, std::memory_order_release);
    const uint64_t cpu = t.cpu_ns.load(std::memory_order_relaxed);
    const double cpu_share = window_seconds > 0 ? double(cpu - t.cpu_ns_reported) / 1e9 / window_seconds : 0;
    t.cpu_ns_reported = cpu;
    if (n < 10 || cpu_share < 0.02) // idle threads: not worth a section
        return;
    const uintptr_t text = reinterpret_cast<uintptr_t>(__text_start);
    int eboot = 0, sys = 0, jit = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] & kCallerTag) {
            s[i] = ((s[i] & ~kCallerTag) - text) | kSysTag | kCallerTag; // exact return address
            sys++;
        } else if (s[i] >= text && s[i] < text + kTextWindow) {
            s[i] = (s[i] - text) & ~0xFULL;
            eboot++;
        } else if (s[i] >= kSystemLibs && s[i] < 2 * kSystemLibs) {
            s[i] = (s[i] & ~0xFFFULL) | kSysTag; // 4 KiB granularity is plenty for libkernel/libc
            sys++;
        } else {
            s[i] = (s[i] & ~0xFULL) | kJitTag;
            jit++;
        }
    }
    std::sort(s, s + n);
    struct Entry {
        uint64_t key;
        int count;
    };
    static Entry entries[kMaxSamples];
    int unique = 0;
    for (int i = 0; i < n;) {
        int j = i;
        while (j < n && s[j] == s[i])
            j++;
        entries[unique++] = {s[i], j - i};
        i = j;
    }
    std::sort(entries, entries + unique, [](const Entry& a, const Entry& b) { return a.count > b.count; });

    char line[256];
    snprintf(line, sizeof(line), "== %.0f s, %s: %d samples, CPU %.0f%% busy: jit %.0f%%, eden %.0f%%, sys %.0f%%\n",
             window_start, t.name, n, 100.0 * cpu_share, 100.0 * jit / n, 100.0 * eboot / n, 100.0 * sys / n);
    writeLine(fd, line);
    for (int i = 0; i < unique && i < 400 && entries[i].count >= 2; i++) {
        const uint64_t key = entries[i].key;
        const char* kind = (key & kJitTag) ? "jit" : (key & kCallerTag) ? "sys<-elf" : (key & kSysTag) ? "sys" : "elf";
        snprintf(line, sizeof(line), "  %5.1f%% %s 0x%llx\n", 100.0 * entries[i].count / n, kind,
                 static_cast<unsigned long long>(key & ~(kJitTag | kSysTag | kCallerTag)));
        writeLine(fd, line);
    }
}

double now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

void* samplerThread(void*) {
    ps4_sampler_register_thread("sampler");
    const int fd = open("/data/switch-emu/logs/samples.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return nullptr;
    char header[128];
    snprintf(header, sizeof(header), "eden sampler: 1 kHz, .text at %p\n", static_cast<void*>(__text_start));
    writeLine(fd, header);
    const double start = now();
    double window_start = start;
    const timespec period = {0, 1000000};
    for (;;) {
        nanosleep(&period, nullptr);
        const int threads = g_thread_count.load(std::memory_order_acquire);
        for (int i = 0; i < threads; i++)
            pthread_kill(g_threads[i].thread.load(std::memory_order_relaxed), kSigProf);
        if (now() - window_start >= 10.0) {
            const double window_seconds = now() - window_start;
            for (int i = 0; i < threads; i++)
                report(fd, g_threads[i], window_start - start, window_seconds);
            window_start = now();
        }
    }
    return nullptr;
}

} // namespace

// The calling thread's name as registered below (for crash.log), or nullptr.
extern "C" const char* ps4_sampler_thread_name() {
    const pthread_t self = pthread_self();
    const int threads = g_thread_count.load(std::memory_order_acquire);
    for (int i = 0; i < threads; i++)
        if (g_threads[i].thread.load(std::memory_order_relaxed) == self)
            return g_threads[i].name;
    return nullptr;
}

// Called by Common::SetCurrentThreadName: every named Eden thread can be sampled.
extern "C" void ps4_sampler_register_thread(const char* name) {
    const int index = g_thread_count.load();
    if (index >= kMaxThreads)
        return;
    snprintf(g_threads[index].name, sizeof(g_threads[index].name), "%s", name ? name : "?");
    int here = 0;
    g_threads[index].stack_hint.store(reinterpret_cast<uintptr_t>(&here));
    g_threads[index].thread.store(pthread_self());
    g_thread_count.store(index + 1, std::memory_order_release);
}

extern "C" void ps4_sampler_start() {
    if (g_on.exchange(true))
        return;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    void (*handler)(int, siginfo_t*, void*) = profHandler;
    memcpy(&action, &handler, sizeof(handler)); // the handler union's member names differ
    action.sa_flags = kSaSiginfo | kSaRestart;
    sigemptyset(&action.sa_mask);
    sigaction(kSigProf, &action, nullptr);

    pthread_t thread;
    if (pthread_create(&thread, nullptr, samplerThread, nullptr) == 0)
        ps4_boot_trace("sampler: started, writing /data/switch-emu/logs/samples.log every 10 s");
}

// Hang watchdog: when the main loop stops (ps4_heartbeat not called for 10 s), every registered
// thread is signalled and records where it is - its instruction pointer and the eboot addresses
// on its stack - and the result goes to /data/switch-emu/logs/hang.log. A whole-app freeze (every
// thread, the sampler included, stopped with nothing logged) is usually a lock that a thread holds
// forever; this shows which thread and where. Only async-signal-safe calls from the handler and
// the watchdog (no printf: FreeBSD's formats doubles with malloc, which may be the lock held).
namespace {
constexpr int kSigHang = 31; // FreeBSD SIGUSR2
constexpr int kHangFrames = 40;
struct HangSlot {
    std::atomic<int> state{0}; // 0 idle, 1 asked, 2 recorded
    uint64_t rip = 0, rsp = 0, cpu_ms = 0;
    uint64_t frames[kHangFrames];
    int frame_count = 0;
};
HangSlot g_hang[kMaxThreads];
std::atomic<uint64_t> g_heartbeat{0};

extern "C" int32_t sceKernelQueryMemoryProtection(void* address, void** start, void** end, uint32_t* prot);
// The kernel log (GoldHEN streams it on port 3232): no file system involved, so it still gets out
// when file writes are what's stuck.
extern "C" int32_t sceKernelDebugOutText(int32_t channel, const char* text, ...);
void klog(const char* text) {
    sceKernelDebugOutText(0, "[eden] ");
    sceKernelDebugOutText(0, text);
}

void hangHandler(int, siginfo_t*, void* context) {
    const pthread_t self = pthread_self();
    const int threads = g_thread_count.load(std::memory_order_acquire);
    for (int i = 0; i < threads; i++) {
        if (g_threads[i].thread.load(std::memory_order_relaxed) != self)
            continue;
        HangSlot& h = g_hang[i];
        const auto* regs = reinterpret_cast<const uint64_t*>(static_cast<char*>(context) + kMcontextOffset);
        h.rip = regs[kRipIndex];
        h.rsp = regs[kRipIndex + 3]; // mcontext: rip, cs, rflags, rsp
        timespec cpu;
        h.cpu_ms = clock_gettime(kThreadCpuClock, &cpu) == 0 ? uint64_t(cpu.tv_sec) * 1000 + uint64_t(cpu.tv_nsec) / 1000000 : 0;
        const uintptr_t text = reinterpret_cast<uintptr_t>(__text_start);
        int words = 8192; // 64 KiB of stack, or to the end of its mapping
        void* map_start = nullptr;
        void* map_end = nullptr;
        uint32_t prot = 0;
        if (sceKernelQueryMemoryProtection(reinterpret_cast<void*>(h.rsp), &map_start, &map_end, &prot) == 0) {
            const uint64_t avail = (reinterpret_cast<uint64_t>(map_end) - h.rsp) / 8;
            if (avail < uint64_t(words))
                words = int(avail);
        } else {
            words = 0;
        }
        const auto* stack = reinterpret_cast<const uint64_t*>(h.rsp);
        int n = 0;
        for (int w = 0; w < words && n < kHangFrames; w++)
            if (stack[w] >= text && stack[w] < text + kTextWindow)
                h.frames[n++] = stack[w] - text;
        h.frame_count = n;
        h.state.store(2, std::memory_order_release);
        return;
    }
}

// Appends text / a hex number to buf (no libc formatting).
size_t put(char* buf, size_t at, size_t cap, const char* text) {
    while (*text && at + 1 < cap)
        buf[at++] = *text++;
    return at;
}
size_t putHex(char* buf, size_t at, size_t cap, uint64_t v) {
    char tmp[19] = "0x";
    int len = 2;
    char digits[16];
    int d = 0;
    do {
        digits[d++] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v);
    while (d)
        tmp[len++] = digits[--d];
    tmp[len] = 0;
    return put(buf, at, cap, tmp);
}

size_t putDec(char* buf, size_t at, size_t cap, uint64_t v) {
    char digits[21];
    int d = 0;
    do {
        digits[d++] = char('0' + v % 10);
        v /= 10;
    } while (v);
    char tmp[21];
    int len = 0;
    while (d)
        tmp[len++] = digits[--d];
    tmp[len] = 0;
    return put(buf, at, cap, tmp);
}

void dumpHang(uint64_t seconds_stuck) {
    const int threads = g_thread_count.load(std::memory_order_acquire);
    const pthread_t self = pthread_self();
    for (int i = 0; i < threads; i++) {
        const pthread_t t = g_threads[i].thread.load(std::memory_order_relaxed);
        if (t == self)
            continue;
        g_hang[i].state.store(1, std::memory_order_release);
        pthread_kill(t, kSigHang);
    }
    const timespec wait = {0, 300000000};
    nanosleep(&wait, nullptr);
    // The kernel log first: opening the file may be what blocks.
    static char buf[4096];
    for (int pass = 0; pass < 2; pass++) {
    const int fd = pass == 0 ? -2 : open("/data/switch-emu/logs/hang.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd == -1)
        return;
    const auto emit = [fd](const char* text) {
        if (fd == -2)
            klog(text);
        else
            writeLine(fd, text);
    };
    size_t at = put(buf, 0, sizeof(buf), "=== host threads, stuck for ");
    at = putDec(buf, at, sizeof(buf), seconds_stuck);
    at = put(buf, at, sizeof(buf), " s; text at ");
    at = putHex(buf, at, sizeof(buf), reinterpret_cast<uintptr_t>(__text_start));
    at = put(buf, at, sizeof(buf), " (addresses below: elf; sys = system library)\n");
    emit(buf);
    const uintptr_t text = reinterpret_cast<uintptr_t>(__text_start);
    for (int i = 0; i < threads; i++) {
        if (g_threads[i].thread.load(std::memory_order_relaxed) == self)
            continue;
        const HangSlot& h = g_hang[i];
        at = put(buf, 0, sizeof(buf), g_threads[i].name);
        if (h.state.load(std::memory_order_acquire) != 2) {
            at = put(buf, at, sizeof(buf), ": did not answer (signals blocked, or stopped)\n");
            emit(buf);
            continue;
        }
        at = put(buf, at, sizeof(buf), ": cpu ms ");
        at = putDec(buf, at, sizeof(buf), h.cpu_ms);
        at = put(buf, at, sizeof(buf), ", rip ");
        const bool in_text = h.rip >= text && h.rip < text + kTextWindow;
        at = put(buf, at, sizeof(buf), in_text ? "elf " : (h.rip >= kSystemLibs && h.rip < 2 * kSystemLibs) ? "sys " : "jit ");
        at = putHex(buf, at, sizeof(buf), in_text ? h.rip - text : h.rip);
        at = put(buf, at, sizeof(buf), ", stack:");
        for (int f = 0; f < h.frame_count; f++) {
            at = put(buf, at, sizeof(buf), " ");
            at = putHex(buf, at, sizeof(buf), h.frames[f]);
        }
        at = put(buf, at, sizeof(buf), "\n");
        emit(buf);
    }
    if (fd >= 0)
        close(fd);
    }
}

extern "C" void* scePthreadSelf();
extern "C" int scePthreadSetprio(void* thread, int priority);

void* watchdogThread(void*) {
    ps4_sampler_register_thread("watchdog");
    // Above everything else (256 is the PS4's highest): a stall where busy threads starve the
    // normal-priority ones would starve a normal-priority watchdog too.
    scePthreadSetprio(scePthreadSelf(), 256);
    klog("watchdog: started\n");
    // Seconds by the clock, not by counting sleeps: the sampler's SIGPROF cuts every sleep short.
    uint64_t last = g_heartbeat.load(std::memory_order_relaxed);
    double last_change = now();
    uint64_t reported = 0; // whole seconds of the current stall already reported
    int dumps = 0;
    for (;;) {
        const double target = now() + 1.0;
        while (now() < target) {
            const timespec rest = {0, 200000000};
            nanosleep(&rest, nullptr);
        }
        const uint64_t beat = g_heartbeat.load(std::memory_order_relaxed);
        if (beat != last) {
            last = beat;
            last_change = now();
            reported = 0;
            continue;
        }
        const uint64_t still = uint64_t(now() - last_change);
        if (still >= 5 && still / 5 > reported / 5) {
            char line[64];
            size_t at = put(line, 0, sizeof(line), "watchdog: main loop stuck ");
            at = putDec(line, at, sizeof(line), still);
            put(line, at, sizeof(line), " s\n");
            klog(line);
        }
        // Dump at 10, 40 and 100 s of no progress (what moves between dumps: busy or stuck).
        for (const uint64_t at : {uint64_t(10), uint64_t(40), uint64_t(100)})
            if (reported < at && still >= at && dumps < 6) {
                dumps++;
                dumpHang(still);
            }
        reported = still;
    }
    return nullptr;
}
} // namespace

// Where every registered (host) thread is right now, to logs/hang.log and the kernel log - for a
// game that stopped while the emulator around it keeps running (yuzu.cpp's game-stall dump).
extern "C" void ps4_dump_host_threads(unsigned seconds) {
    static bool installed = false;
    if (!installed) {
        installed = true;
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        void (*handler)(int, siginfo_t*, void*) = hangHandler;
        memcpy(&action, &handler, sizeof(handler));
        action.sa_flags = kSaSiginfo | kSaRestart;
        sigemptyset(&action.sa_mask);
        sigaction(kSigHang, &action, nullptr);
    }
    dumpHang(seconds);
}

// The main loop calls this every iteration.
extern "C" void ps4_heartbeat() {
    g_heartbeat.fetch_add(1, std::memory_order_relaxed);
}

// Starts the hang watchdog (from the main thread, which registers itself as "main").
extern "C" void ps4_hang_watchdog_start() {
    static bool started = false;
    if (started)
        return;
    started = true;
    ps4_sampler_register_thread("main");
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    void (*handler)(int, siginfo_t*, void*) = hangHandler;
    memcpy(&action, &handler, sizeof(handler));
    action.sa_flags = kSaSiginfo | kSaRestart;
    sigemptyset(&action.sa_mask);
    sigaction(kSigHang, &action, nullptr);
    pthread_t thread;
    pthread_create(&thread, nullptr, watchdogThread, nullptr);
}
