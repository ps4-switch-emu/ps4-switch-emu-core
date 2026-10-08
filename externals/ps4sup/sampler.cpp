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

namespace {

constexpr int kSigProf = 27; // FreeBSD value
constexpr int kSaSiginfo = 0x40, kSaRestart = 0x0002;
constexpr int kMcontextOffset = 0x40; // measured on hardware (see crashlog.cpp)
constexpr int kRipIndex = 20;
constexpr uint64_t kSystemLibs = 0x800000000ULL, kSysTag = 1ULL << 62, kJitTag = 1ULL << 63;
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
            t.samples[n] = regs[kRipIndex];
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
        if (s[i] >= text && s[i] < text + kTextWindow) {
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
        const char* kind = (key & kJitTag) ? "jit" : (key & kSysTag) ? "sys" : "elf";
        snprintf(line, sizeof(line), "  %5.1f%% %s 0x%llx\n", 100.0 * entries[i].count / n, kind,
                 static_cast<unsigned long long>(key & ~(kJitTag | kSysTag)));
        writeLine(fd, line);
    }
}

double now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

void* samplerThread(void*) {
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

// Called by Common::SetCurrentThreadName: every named Eden thread can be sampled.
extern "C" void ps4_sampler_register_thread(const char* name) {
    const int index = g_thread_count.load();
    if (index >= kMaxThreads)
        return;
    snprintf(g_threads[index].name, sizeof(g_threads[index].name), "%s", name ? name : "?");
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
