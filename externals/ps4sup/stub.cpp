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
#include <atomic>
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
