// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* virtual_buffer.cpp */
// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#include <windows.h>
#include <mutex>
#else
#include <sys/mman.h>
#endif

#ifdef __OPENORBIS__
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
extern "C" __attribute__((weak)) void ps4_boot_trace(const char* stage); // externals/ps4sup/crashlog.cpp
#include <ranges>
#include <csignal>
#include <fcntl.h>
#include <boost/container/static_vector.hpp>
#include <orbis/SystemService.h>
#include <orbis/libkernel.h>
#endif

#include "common/alignment.h"
#include "common/assert.h"
#include "common/sparse_large_vector.h"

// PlayStation 4
// Flag needs to be undef-ed on non PS4 since it has different semantics
// on some platforms.
#ifdef __OPENORBIS__
#   ifndef MAP_SYSTEM
#       define MAP_SYSTEM 0x2000
#   endif
#   ifndef MAP_VOID
#       define MAP_VOID 0x100
#   endif
#endif

namespace Common {

#ifdef _WIN32
static std::vector<std::pair<u64, u64>> vector_regions {};

// Workaround for handling non-commited memory accessed by Dynarmic; usually result of an error
static LONG WINAPI FakePageFaultHandler(PEXCEPTION_POINTERS info) {
    DWORD code = info->ExceptionRecord->ExceptionCode;
    u64 exception_addr = reinterpret_cast<u64>(info->ExceptionRecord->ExceptionAddress);

    if (code != EXCEPTION_ACCESS_VIOLATION) {
        // Not our problem
        return EXCEPTION_CONTINUE_SEARCH;
    }

    u64 addr = 0, addr2 = 0;

    for (auto region: vector_regions) {
        auto addr_shifted = exception_addr >> HostPageBits;
        if (region.first <= addr_shifted && addr_shifted <= region.second) {
            addr = addr_shifted;
        }

        // Page-boundary accesses
        if (auto addr_ = (exception_addr + 0x40) >> HostPageBits; addr_ != addr_shifted && region.first <= addr_ && addr_ <= region.second) {
            addr2 = addr_;
        }

        if (addr != 0 || addr2 != 0) {
            break;
        }
    }

    if (addr == 0 && addr2 == 0) {
        // Not our problem
        return EXCEPTION_CONTINUE_SEARCH;
    }

    LOG_ERROR(HW_Memory, "Accessing an unallocated region of a SparseLargeVector at {:#x}; this shouldn't happen and is likely a Dynarmic error!", exception_addr);

    // Commit this region
    if (addr != 0) {
        if (!CommitVectorPage(addr << HostPageBits, false)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    // Commit next region if needed
    if (addr2 != 0) {
        if (!CommitVectorPage(addr2 << HostPageBits, false)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    return EXCEPTION_CONTINUE_EXECUTION;
}

bool CommitVectorPage(uintptr_t addr, bool write) noexcept {
    MEMORY_BASIC_INFORMATION info {};
    auto res = VirtualQuery(reinterpret_cast<void*>(addr), &info, sizeof(info));
    if (res == 0) {
        LOG_CRITICAL(HW_Memory, "Failed to query large buffer region at {:#x} with error {}, will try committing anyway", addr, GetLastError());
    } else if (info.State != MEM_RESERVE) {
        LOG_ERROR(HW_Memory, "Tried to commit an unreserved large buffer region at {:#x} that is not mapped or is already committed (state {:#x})", addr, info.State);
        return false;
    }

    auto perm = write ? PAGE_READWRITE : PAGE_READONLY;
    void* res2 = VirtualAlloc(reinterpret_cast<LPVOID>(addr), HostPageSize, MEM_COMMIT, perm);
    if (res2 == nullptr) {
        LOG_ERROR(HW_Memory, "Failed to commit large buffer region at {:#x}, error {}", addr, GetLastError());
        return false;
    }

    return true;
}
#endif

#ifndef MAP_NOCORE
#define MAP_NOCORE 0
#endif

#ifdef __OPENORBIS__

namespace Orbis {
constexpr size_t ORBIS_PAGE_SIZE = 16384;

struct Ucontext {
    struct Sigset {
        u64 bits[2];
    } uc_sigmask;
    int field1_0x10[12];
    struct Mcontext {
        u64 mc_onstack;
        u64 mc_rdi;
        u64 mc_rsi;
        u64 mc_rdx;
        u64 mc_rcx;
        u64 mc_r8;
        u64 mc_r9;
        u64 mc_rax;
        u64 mc_rbx;
        u64 mc_rbp;
        u64 mc_r10;
        u64 mc_r11;
        u64 mc_r12;
        u64 mc_r13;
        u64 mc_r14;
        u64 mc_r15;
        int mc_trapno;
        u16 mc_fs;
        u16 mc_gs;
        u64 mc_addr;
        int mc_flags;
        u16 mc_es;
        u16 mc_ds;
        u64 mc_err;
        u64 mc_rip;
        u64 mc_cs;
        u64 mc_rflags;
        u64 mc_rsp;
        u64 mc_ss;
        u64 mc_len;
        u64 mc_fpformat;
        u64 mc_ownedfp;
        u64 mc_lbrfrom;
        u64 mc_lbrto;
        u64 mc_aux1;
        u64 mc_aux2;
        u64 mc_fpstate[104];
        u64 mc_fsbase;
        u64 mc_gsbase;
        u64 mc_spare[6];
    } uc_mcontext;
    struct Ucontext* uc_link;
    struct ExStack {
        void* ss_sp;
        std::size_t ss_size;
        int ss_flags;
        int _align;
    } uc_stack;
    int uc_flags;
    int __spare[4];
    int field7_0x4f4[3];
};
}

static boost::container::static_vector<std::pair<void*, size_t>, 16> swap_regions;
static std::mutex evil_swap_mutex;
static struct sigaction g_prev_segv, g_prev_bus;
static unsigned swap_traced; // trace only the first few unhandled faults
// Direct-memory large vectors: {mapping, direct memory offset, size}, to release them again.
struct DmemRegion {
    void* base;
    off_t offset;
    size_t size;
};
static std::vector<DmemRegion> dmem_regions;

// Faults inside a "void" region get a 2 MiB page mapped in on demand; any other fault goes to the
// handler installed before ours (the PS4 crash logger, externals/ps4sup/crashlog.cpp).
// A plain sigaction handler (the libc-overlay headers give the PS4 kernel's FreeBSD layout), as
// DolphinPS4 uses on retail consoles, instead of sceKernelInstallExceptionHandler.
extern "C" bool ps4_dram_fault(uintptr_t address); // common/host_memory.cpp

static void SwapHandler(int sig, siginfo_t* info, void* raw_context) {
    const uintptr_t addr = uintptr_t(info->si_addr);
    if (ps4_dram_fault(addr))
        return; // emulated DRAM chunk committed: retry the access
    {
        std::unique_lock lk{evil_swap_mutex};
        if (auto const it = std::ranges::find_if(swap_regions, [addr](auto const& e) {
            return addr >= uintptr_t(e.first) && addr < uintptr_t(e.first) + e.second;
        }); it != swap_regions.end()) {
            size_t const page_size = Orbis::ORBIS_PAGE_SIZE * 128;
            void* aligned_addr = reinterpret_cast<void*>(addr & ~(page_size - 1));
            void* res = mmap(aligned_addr, page_size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
            if (res != MAP_FAILED)
                return; // retry the access
            if (ps4_boot_trace && swap_traced++ < 16) {
                char line[160];
                snprintf(line, sizeof(line), "swap: fault at %#lx in void region %p+%#zx, mapping 2 MiB failed (errno %d)",
                         (unsigned long)addr, it->first, it->second, errno);
                ps4_boot_trace(line);
            }
        } else if (ps4_boot_trace && swap_traced++ < 16) {
            char line[512];
            int n = snprintf(line, sizeof(line), "swap: fault at %#lx (signal %d) outside the %zu void regions:", (unsigned long)addr,
                             sig, swap_regions.size());
            for (auto const& e : swap_regions)
                n += snprintf(line + n, sizeof(line) - size_t(n), " %p+%#zx", e.first, e.second);
            ps4_boot_trace(line);
        }
    }
    // Not ours (or the page couldn't be mapped): chain.
    const struct sigaction& prev = sig == SIGSEGV ? g_prev_segv : g_prev_bus;
    if ((prev.sa_flags & SA_SIGINFO) && prev.sa_sigaction) {
        prev.sa_sigaction(sig, info, raw_context);
    } else if (prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN && prev.sa_handler) {
        prev.sa_handler(sig);
    } else {
        signal(sig, SIG_DFL); // returning re-faults with the default action
    }
}
void InitSwap() noexcept {
    struct sigaction sa{};
    sa.sa_sigaction = SwapHandler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_prev_segv);
    sigaction(SIGBUS, &sa, &g_prev_bus);
}
#else
void InitSwap() noexcept {}
#endif

void* AllocateMemoryPages(std::size_t size) noexcept {
    if (auto page = HostPageSize; size % page != 0) {
        LOG_WARNING(HW_Memory, "Allocating unaligned large vector with size {:#x}; aligning to {} page size", size, page);
        size = AlignUp(size, page);
    }

#ifdef _WIN32
    // We will never use this memory entirely so instead of committing it up front let's just reserve it and commit each page individually
    void* base = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE);

    if (base != nullptr) {
        vector_regions.emplace_back(reinterpret_cast<u64>(base), reinterpret_cast<u64>(base) + size);

        static std::once_flag flag;
        std::call_once(flag, []() { AddVectoredExceptionHandler(1, FakePageFaultHandler); });
    } else {
        // Try committing everything instead??
        LOG_WARNING(HW_Memory, "Failed to reserve large vector region with error {}, trying to commit instead..", GetLastError());
        base = VirtualAlloc(nullptr, size, MEM_COMMIT, PAGE_READWRITE);
    }
    ASSERT_MSG(base, "Failed to reserve {:#x} sized region with error {}", size, GetLastError());
#elif defined(__OPENORBIS__)
    bool use_void_mem = true;
    void* base = nullptr;
    // Direct memory is what RADV allocates from too: only small vectors get it. Big sparse
    // ones (the 1 GiB CPU page table) are backed on demand, 2 MiB at a time.
    if (size <= 128ull << 20) {
        size_t align = Orbis::ORBIS_PAGE_SIZE;
        off_t offset;
        int32_t res;
        size = (size + align - 1) / align * align;
        if ((res = sceKernelAllocateDirectMemory(0, ORBIS_KERNEL_MAIN_DMEM_SIZE, size, align, ORBIS_KERNEL_WB_ONION, &offset)) == 0) {
            if ((res = sceKernelMapDirectMemory(&base, size, ORBIS_KERNEL_PROT_CPU_READ | ORBIS_KERNEL_PROT_CPU_WRITE, 0, offset, size)) == 0) {
                if ((res = sceKernelMprotect(base, size, VM_PROT_ALL)) == 0 && base != nullptr) {
                    // Direct memory is handed out with whatever the last user left in it (seen on
                    // hardware: VideoOut pixels in DeviceMemoryManager's compressed_device_addr,
                    // read back as a multi-address chain id). Every caller expects zeroed pages.
                    std::memset(base, 0, size);
                    {
                        std::scoped_lock lk{evil_swap_mutex};
                        dmem_regions.push_back({base, offset, size});
                    }
                    LOG_WARNING(HW_Memory, "Using DMem for {} bytes area @ {}", size, base);
                    use_void_mem = false; //Memory properly mapped
                } else {
                    sceKernelReleaseDirectMemory(offset, size);
                    LOG_ERROR(HW_Memory, "{} = sceKernelMprotect({}, {})", res, offset, size);
                }
            } else {
                sceKernelReleaseDirectMemory(offset, size);
                LOG_ERROR(HW_Memory, "{} = sceKernelMapDirectMemory({}, {})", res, offset, size);
            }
        } else {
            LOG_ERROR(HW_Memory, "{} = sceKernelAllocateDirectMemory({}, {}, {})", res, size, align, offset);
        }
    }
    if (use_void_mem) {
        base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_VOID | MAP_PRIVATE, -1, 0);
        LOG_WARNING(HW_Memory, "Using VoidMem for {} bytes area @ {}", size, base);
        ASSERT(base != MAP_FAILED);
        ASSERT_MSG(swap_regions.size() < swap_regions.capacity(), "too many void regions");
        swap_regions.emplace_back(base, size);
    }
    if (ps4_boot_trace) {
        char line[160];
        snprintf(line, sizeof(line), "large vector: %zu KiB from %s at %p", size >> 10, use_void_mem ? "void memory" : "direct memory",
                 base);
        ps4_boot_trace(line);
    }
#else
    void* base = mmap(nullptr, size, PROT_READ, MAP_ANON | MAP_PRIVATE | MAP_NOCORE, -1, 0);
    if (base == MAP_FAILED)
        base = nullptr;
    ASSERT_MSG(base, "Failed to allocate {:#x} sized region with error {}", size, strerror(errno));
#endif
    return base;
}

void FreeMemoryPages(void* base, [[maybe_unused]] std::size_t size) noexcept {
    if (auto page = HostPageSize; size % page != 0) {
        size = AlignUp(size, page);
    }
    if (!base)
        return;
#ifdef _WIN32
    ASSERT(VirtualFree(base, 0, MEM_RELEASE));
#elif defined(__OPENORBIS__)
    std::scoped_lock lk{evil_swap_mutex};
    if (auto const it = std::ranges::find_if(swap_regions, [=](auto const& e) {
        return uintptr_t(base) >= uintptr_t(e.first) && uintptr_t(base) < uintptr_t(e.first) + e.second;
    }); it != swap_regions.end()) {
        int rc = munmap(base, size);
        ASSERT(rc == 0);
        swap_regions.erase(it);
    } else if (auto const dm = std::ranges::find_if(dmem_regions, [=](auto const& e) {
        return e.base == base;
    }); dm != dmem_regions.end()) {
        // Unmap, then release by direct memory offset (not by address: that released nothing
        // and leaked every freed vector, 8 MiB per guest program on hardware).
        munmap(dm->base, dm->size);
        const int rc = sceKernelReleaseDirectMemory(dm->offset, dm->size);
        if (rc != 0)
            LOG_ERROR(HW_Memory, "{:#x} = sceKernelReleaseDirectMemory({:#x}, {:#x})", u32(rc),
                      dm->offset, dm->size);
        dmem_regions.erase(dm);
    } else {
        LOG_ERROR(HW_Memory, "FreeMemoryPages: {} was not allocated here", base);
    }
#else
    ASSERT(munmap(base, size) == 0);
#endif
}

} // namespace Common
