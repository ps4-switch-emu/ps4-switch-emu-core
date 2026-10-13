// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <errno.h>
#include <stddef.h>
#include <stdio.h>

#define STUB_WEAK(name) \
    extern "C" void name() { \
        printf("called " #name); \
        asm volatile("ud2"); \
    }

#define STUB_ZERO(name) \
    extern "C" int name() { \
        printf("called " #name " = ret zero"); \
        return 0; \
    }


// __cxa_thread_atexit_impl: real implementation in dps4/ps4_runtime.cpp.

// getentropy(): referenced weakly (so not pulled in automatically) and missing from the OpenOrbis
// libc, which create-fself rejects as an unresolved import. Backed by the system RNG, which
// returns at most 64 bytes per call.
extern "C" int sceRandomGetRandomNumber(void* buf, size_t size);
extern "C" int getentropy(void* buffer, size_t length) {
    if (length > 256) {
        errno = EIO;
        return -1;
    }
    auto* out = static_cast<unsigned char*>(buffer);
    while (length > 0) {
        const size_t chunk = length < 64 ? length : 64;
        if (sceRandomGetRandomNumber(out, chunk) != 0) {
            errno = EIO;
            return -1;
        }
        out += chunk;
        length -= chunk;
    }
    return 0;
}

STUB_WEAK(ZSTD_trace_compress_begin)
STUB_WEAK(ZSTD_trace_compress_end)
STUB_WEAK(ZSTD_trace_decompress_begin)
STUB_WEAK(ZSTD_trace_decompress_end)

extern "C" thread_local struct {
    char pad[512];
} _ThreadRuneLocale = {};
extern "C" struct {
    char pad[512];
} __h_errno = {};

FILE* __stderrp = stdout;
FILE* __stdinp = stdin;

#undef STUB_WEAK

// THIS MAKES STD::COUT AND SUCH WORK :)
#include <iostream>
std::ios_base::Init init;

// Lag spike diagnostics (vk_swapchain.cpp reports them with long frames): fastmem faults that
// dynarmic handled, and new pipelines built by the Vulkan pipeline cache.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <time.h>
std::atomic<unsigned long long> ps4_fastmem_faults{0};
std::atomic<unsigned long long> ps4_pipelines_built{0};
extern "C" void ps4_count_fastmem_fault() {
    ps4_fastmem_faults.fetch_add(1, std::memory_order_relaxed);
}

// dynarmic: a core's code cache filled up and is thrown away (everything recompiles: a hitch).
extern "C" void ps4_boot_trace(const char* stage);
extern "C" void ps4_jit_cache_full(unsigned core, unsigned long long size) {
    static std::atomic<unsigned> count{0};
    char line[128];
    snprintf(line, sizeof(line), "jit: core %u code cache full (%llu MiB), cleared - %u so far", core,
             size >> 20, count.fetch_add(1, std::memory_order_relaxed) + 1);
    ps4_boot_trace(line);
}

// The CPU's clock, measured (~50 ms, once): a chain of dependent adds runs at one per cycle, so
// adds per second is the clock. A PS4 Pro runs its cores at ~2.1 GHz instead of 1.6 in its
// enhanced mode (the PRO package) or with Boost Mode on - this shows which one a run got.
extern "C" int32_t sceKernelIsNeoMode();
extern "C" unsigned ps4_cpu_mhz() {
    static unsigned mhz = 0;
    if (mhz != 0)
        return mhz;
    const auto run = [](unsigned long long iterations) {
        timespec t0{}, t1{};
        unsigned long long x = 0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned long long i = 0; i < iterations; i++) {
            asm volatile("add $1, %0\n\tadd $1, %0\n\tadd $1, %0\n\tadd $1, %0\n\t"
                         "add $1, %0\n\tadd $1, %0\n\tadd $1, %0\n\tadd $1, %0"
                         : "+r"(x));
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        return double(t1.tv_sec - t0.tv_sec) + double(t1.tv_nsec - t0.tv_nsec) * 1e-9;
    };
    run(1'000'000); // warm up (the core may be clocked down while idle)
    constexpr unsigned long long kIterations = 12'000'000; // 96M adds: ~45 ms at 2.1 GHz
    double best = 1e9;
    for (int i = 0; i < 3; i++)
        best = std::min(best, run(kIterations));
    mhz = unsigned(double(kIterations) * 8.0 / best / 1e6 + 0.5);
    return mhz;
}

extern "C" bool ps4_is_pro_mode() {
    return sceKernelIsNeoMode() != 0;
}

// Free direct memory (the largest free block), refreshed at most twice a second: GPU memory and
// the emulated DRAM come out of the same pool, so the texture cache trims itself by what's left
// rather than by the GPU's nominal size (video_core/texture_cache/texture_cache.h).
extern "C" int32_t sceKernelAvailableDirectMemorySize(off_t start, off_t end, size_t alignment, off_t* out_start,
                                                      size_t* out_size);
extern "C" size_t sceKernelGetDirectMemorySize();
extern "C" unsigned long long ps4_direct_memory_free() {
    static std::atomic<unsigned long long> s_free{~0ull};
    static std::atomic<long long> s_when{0};
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const long long ms = (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    if (ms - s_when.load(std::memory_order_relaxed) >= 500) {
        s_when.store(ms, std::memory_order_relaxed);
        off_t at = 0;
        size_t size = 0;
        if (sceKernelAvailableDirectMemorySize(0, off_t(sceKernelGetDirectMemorySize()), 16 * 1024, &at, &size) == 0)
            s_free.store(size, std::memory_order_relaxed);
    }
    return s_free.load(std::memory_order_relaxed);
}
