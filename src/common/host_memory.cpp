// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32

#include <iterator>
#include "common/container/unordered_map.h"
#include <boost/icl/separate_interval_set.hpp>
#include <windows.h>
#include "common/dynamic_library.h"

#else // ^^^ Windows ^^^ vvv POSIX vvv

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <boost/icl/interval_set.hpp>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "common/scope_exit.h"

#if defined(__linux__)
#include <sys/random.h>
#elif defined(__APPLE__)
#include <sys/types.h>
#include <sys/random.h>
#include <mach/vm_map.h>
#include <mach/mach.h>
#elif defined(__FreeBSD__)
#include <sys/shm.h>
#elif defined(__OPENORBIS__)
#include <orbis/libkernel.h>
#include <sys/stat.h>
#include <atomic>
#include <cstdarg>
#include <ctime>
#include <cstring>
#include <vector>
#include <cstdio>
#include <unordered_map>
#endif

// FreeBSD
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
// Solaris 11 and illumos
#ifndef MAP_ALIGNED_SUPER
#define MAP_ALIGNED_SUPER 0
#endif
// macOS
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#endif // ^^^ POSIX ^^^

#include <algorithm>
#include <mutex>
#include <random>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/free_region_manager.h"
#include "common/host_memory.h"
#include "common/logging.h"

#if defined(__ANDROID__) && __ANDROID_API__ < 30
#include <sys/syscall.h>
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
static int memfd_create(const char* name, unsigned int flags) {
    return syscall(__NR_memfd_create, name, flags);
}
#endif

namespace Common {

[[maybe_unused]] constexpr size_t PageAlignment = 0x1000;
[[maybe_unused]] constexpr size_t HugePageSize = 0x200000;

#ifdef _WIN32

// Manually imported for MinGW compatibility
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif

using PFN_CreateFileMapping2 = _Ret_maybenull_ HANDLE(WINAPI*)(
    _In_ HANDLE File, _In_opt_ SECURITY_ATTRIBUTES* SecurityAttributes, _In_ ULONG DesiredAccess,
    _In_ ULONG PageProtection, _In_ ULONG AllocationAttributes, _In_ ULONG64 MaximumSize,
    _In_opt_ PCWSTR Name,
    _Inout_updates_opt_(ParameterCount) MEM_EXTENDED_PARAMETER* ExtendedParameters,
    _In_ ULONG ParameterCount);

using PFN_VirtualAlloc2 = _Ret_maybenull_ PVOID(WINAPI*)(
    _In_opt_ HANDLE Process, _In_opt_ PVOID BaseAddress, _In_ SIZE_T Size,
    _In_ ULONG AllocationType, _In_ ULONG PageProtection,
    _Inout_updates_opt_(ParameterCount) MEM_EXTENDED_PARAMETER* ExtendedParameters,
    _In_ ULONG ParameterCount);

using PFN_MapViewOfFile3 = _Ret_maybenull_ PVOID(WINAPI*)(
    _In_ HANDLE FileMapping, _In_opt_ HANDLE Process, _In_opt_ PVOID BaseAddress,
    _In_ ULONG64 Offset, _In_ SIZE_T ViewSize, _In_ ULONG AllocationType, _In_ ULONG PageProtection,
    _Inout_updates_opt_(ParameterCount) MEM_EXTENDED_PARAMETER* ExtendedParameters,
    _In_ ULONG ParameterCount);

using PFN_UnmapViewOfFile2 = BOOL(WINAPI*)(_In_ HANDLE Process, _In_ PVOID BaseAddress,
                                           _In_ ULONG UnmapFlags);

template <typename T>
static void GetFuncAddress(Common::DynamicLibrary& dll, const char* name, T& pfn) {
    if (!dll.GetSymbol(name, &pfn)) {
        LOG_CRITICAL(HW_Memory, "Failed to load {}", name);
    }
}

class HostMemory::Impl {
public:
    explicit Impl(size_t backing_size_, size_t virtual_size_)
        : backing_size{backing_size_}
        , virtual_size{virtual_size_}
        , process{GetCurrentProcess()}
        , kernelbase_dll("Kernelbase")
    {}

    bool Init() {
        if (!kernelbase_dll.IsOpen()) {
            LOG_CRITICAL(HW_Memory, "Failed to load Kernelbase.dll");
            return false;
        }
        GetFuncAddress(kernelbase_dll, "CreateFileMapping2", pfn_CreateFileMapping2);
        GetFuncAddress(kernelbase_dll, "VirtualAlloc2", pfn_VirtualAlloc2);
        GetFuncAddress(kernelbase_dll, "MapViewOfFile3", pfn_MapViewOfFile3);
        GetFuncAddress(kernelbase_dll, "UnmapViewOfFile2", pfn_UnmapViewOfFile2);

        if (!pfn_CreateFileMapping2 || !pfn_VirtualAlloc2 || !pfn_MapViewOfFile3 || !pfn_UnmapViewOfFile2) {
            LOG_CRITICAL(HW_Memory, "Failed to find functions for virtual allocs");
            return false;
        }

        // Allocate backing file map
        backing_handle = pfn_CreateFileMapping2(INVALID_HANDLE_VALUE, nullptr, FILE_MAP_WRITE | FILE_MAP_READ, PAGE_READWRITE, SEC_COMMIT, backing_size, nullptr, nullptr, 0);
        if (!backing_handle) {
            LOG_CRITICAL(HW_Memory, "Failed to allocate {} MiB of backing memory", backing_size >> 20);
            return false;
        }
        // Allocate a virtual memory for the backing file map as placeholder
        backing_base = static_cast<u8*>(pfn_VirtualAlloc2(process, nullptr, backing_size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
        if (!backing_base) {
            Release();
            LOG_CRITICAL(HW_Memory, "Failed to reserve {} MiB of virtual memory", backing_size >> 20);
            return false;
        }
        // Map backing placeholder
        void* const ret = pfn_MapViewOfFile3(backing_handle, process, backing_base, 0, backing_size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
        if (ret != backing_base) {
            Release();
            LOG_CRITICAL(HW_Memory, "Failed to map {} MiB of virtual memory", backing_size >> 20);
            return false;
        }
        // Allocate virtual address placeholder
        virtual_base = static_cast<u8*>(pfn_VirtualAlloc2(process, nullptr, virtual_size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
        if (!virtual_base) {
            Release();
            LOG_CRITICAL(HW_Memory, "Failed to reserve {} GiB of virtual memory", virtual_size >> 30);
            return false;
        }
        return true;
    }

    ~Impl() {
        Release();
    }

    void* Allocate(size_t size) {
        auto* ptr = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (ptr == nullptr) {
            LOG_CRITICAL(HW_Memory, "Failed to allocate fallback buffer with size {:#x}, error {}", size, GetLastError());
        }
        return ptr;
    }

    void Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms) {
        std::unique_lock lock{placeholder_mutex};
        if (!IsNiechePlaceholder(virtual_offset, length)) {
            Split(virtual_offset, length);
        }
        ASSERT(placeholders.find({virtual_offset, virtual_offset + length}) == placeholders.end());
        TrackPlaceholder(virtual_offset, host_offset, length);

        MapView(virtual_offset, host_offset, length);
    }

    void Unmap(size_t virtual_offset, size_t length) {
        std::scoped_lock lock{placeholder_mutex};

        // Unmap until there are no more placeholders
        while (UnmapOnePlaceholder(virtual_offset, length)) {
        }
    }

    void Protect(size_t virtual_offset, size_t length, bool read, bool write, bool execute) {
        DWORD new_flags{};
        if (read && write) {
            new_flags = PAGE_READWRITE;
        } else if (read && !write) {
            new_flags = PAGE_READONLY;
        } else if (!read && !write) {
            new_flags = PAGE_NOACCESS;
        } else {
            UNIMPLEMENTED_MSG("Protection flag combination read={} write={}", read, write);
        }
        const size_t virtual_end = virtual_offset + length;

        std::scoped_lock lock{placeholder_mutex};
        auto [it, end] = placeholders.equal_range({virtual_offset, virtual_end});
        while (it != end) {
            const size_t offset = (std::max)(it->lower(), virtual_offset);
            const size_t protect_length = (std::min)(it->upper(), virtual_end) - offset;
            DWORD old_flags{};
            if (!VirtualProtect(virtual_base + offset, protect_length, new_flags, &old_flags)) {
                LOG_CRITICAL(HW_Memory, "Failed to change virtual memory protect rules");
            }
            ++it;
        }
    }

    void EnableDirectMappedAddress() {
        // TODO
        UNREACHABLE();
    }

    const size_t backing_size; ///< Size of the backing memory in bytes
    const size_t virtual_size; ///< Size of the virtual address placeholder in bytes

    u8* backing_base{};
    u8* virtual_base{};

private:
    /// Release all resources in the object
    void Release() {
        if (!placeholders.empty()) {
            for (const auto& placeholder : placeholders) {
                if (!pfn_UnmapViewOfFile2(process, virtual_base + placeholder.lower(),
                                          MEM_PRESERVE_PLACEHOLDER)) {
                    LOG_CRITICAL(HW_Memory, "Failed to unmap virtual memory placeholder");
                }
            }
            Coalesce(0, virtual_size);
        }
        if (virtual_base) {
            if (!VirtualFree(virtual_base, 0, MEM_RELEASE)) {
                LOG_CRITICAL(HW_Memory, "Failed to free virtual memory");
            }
        }
        if (backing_base) {
            if (!pfn_UnmapViewOfFile2(process, backing_base, MEM_PRESERVE_PLACEHOLDER)) {
                LOG_CRITICAL(HW_Memory, "Failed to unmap backing memory placeholder");
            }
            if (!VirtualFreeEx(process, backing_base, 0, MEM_RELEASE)) {
                LOG_CRITICAL(HW_Memory, "Failed to free backing memory");
            }
        }
        if (!CloseHandle(backing_handle)) {
            LOG_CRITICAL(HW_Memory, "Failed to free backing memory file handle");
        }
    }

    /// Unmap one placeholder in the given range (partial unmaps are supported)
    /// Return true when there are no more placeholders to unmap
    bool UnmapOnePlaceholder(size_t virtual_offset, size_t length) {
        const auto it = placeholders.find({virtual_offset, virtual_offset + length});
        const auto begin = placeholders.begin();
        const auto end = placeholders.end();
        if (it == end) {
            return false;
        }
        const size_t placeholder_begin = it->lower();
        const size_t placeholder_end = it->upper();
        const size_t unmap_begin = (std::max)(virtual_offset, placeholder_begin);
        const size_t unmap_end = (std::min)(virtual_offset + length, placeholder_end);
        ASSERT(unmap_begin >= placeholder_begin && unmap_begin < placeholder_end);
        ASSERT(unmap_end <= placeholder_end && unmap_end > placeholder_begin);

        const auto host_pointer_it = placeholder_host_pointers.find(placeholder_begin);
        ASSERT(host_pointer_it != placeholder_host_pointers.end());
        const size_t host_offset = host_pointer_it->second;

        const bool split_left = unmap_begin > placeholder_begin;
        const bool split_right = unmap_end < placeholder_end;

        if (!pfn_UnmapViewOfFile2(process, virtual_base + placeholder_begin,
                                  MEM_PRESERVE_PLACEHOLDER)) {
            LOG_CRITICAL(HW_Memory, "Failed to unmap placeholder");
        }
        // If we have to remap memory regions due to partial unmaps, we are in a data race as
        // Windows doesn't support remapping memory without unmapping first. Avoid adding any extra
        // logic within the panic region described below.

        // Panic region, we are in a data race right now
        if (split_left || split_right) {
            Split(unmap_begin, unmap_end - unmap_begin);
        }
        if (split_left) {
            MapView(placeholder_begin, host_offset, unmap_begin - placeholder_begin);
        }
        if (split_right) {
            MapView(unmap_end, host_offset + unmap_end - placeholder_begin,
                    placeholder_end - unmap_end);
        }
        // End panic region

        size_t coalesce_begin = unmap_begin;
        if (!split_left) {
            // Try to coalesce pages to the left
            coalesce_begin = it == begin ? 0 : std::prev(it)->upper();
            if (coalesce_begin != placeholder_begin) {
                Coalesce(coalesce_begin, unmap_end - coalesce_begin);
            }
        }
        if (!split_right) {
            // Try to coalesce pages to the right
            const auto next = std::next(it);
            const size_t next_begin = next == end ? virtual_size : next->lower();
            if (placeholder_end != next_begin) {
                // We can coalesce to the right
                Coalesce(coalesce_begin, next_begin - coalesce_begin);
            }
        }
        // Remove and reinsert placeholder trackers
        UntrackPlaceholder(it);
        if (split_left) {
            TrackPlaceholder(placeholder_begin, host_offset, unmap_begin - placeholder_begin);
        }
        if (split_right) {
            TrackPlaceholder(unmap_end, host_offset + unmap_end - placeholder_begin,
                             placeholder_end - unmap_end);
        }
        return true;
    }

    void MapView(size_t virtual_offset, size_t host_offset, size_t length) {
        if (!pfn_MapViewOfFile3(backing_handle, process, virtual_base + virtual_offset, host_offset,
                                length, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0)) {
            LOG_CRITICAL(HW_Memory, "Failed to map placeholder");
        }
    }

    void Split(size_t virtual_offset, size_t length) {
        if (!VirtualFreeEx(process, reinterpret_cast<LPVOID>(virtual_base + virtual_offset), length,
                           MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            LOG_CRITICAL(HW_Memory, "Failed to split placeholder");
        }
    }

    void Coalesce(size_t virtual_offset, size_t length) {
        if (!VirtualFreeEx(process, reinterpret_cast<LPVOID>(virtual_base + virtual_offset), length,
                           MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
            LOG_CRITICAL(HW_Memory, "Failed to coalesce placeholders");
        }
    }

    void TrackPlaceholder(size_t virtual_offset, size_t host_offset, size_t length) {
        placeholders.insert({virtual_offset, virtual_offset + length});
        placeholder_host_pointers.emplace(virtual_offset, host_offset);
    }

    void UntrackPlaceholder(boost::icl::separate_interval_set<size_t>::iterator it) {
        placeholder_host_pointers.erase(it->lower());
        placeholders.erase(it);
    }

    /// Return true when a given memory region is a "nieche" and the placeholders don't have to be
    /// split.
    bool IsNiechePlaceholder(size_t virtual_offset, size_t length) const {
        const auto it = placeholders.upper_bound({virtual_offset, virtual_offset + length});
        if (it != placeholders.end() && it->lower() == virtual_offset + length) {
            return it == placeholders.begin() ? virtual_offset == 0
                                              : std::prev(it)->upper() == virtual_offset;
        }
        return false;
    }

    HANDLE process{};        ///< Current process handle
    HANDLE backing_handle{}; ///< File based backing memory

    DynamicLibrary kernelbase_dll;
    PFN_CreateFileMapping2 pfn_CreateFileMapping2{};
    PFN_VirtualAlloc2 pfn_VirtualAlloc2{};
    PFN_MapViewOfFile3 pfn_MapViewOfFile3{};
    PFN_UnmapViewOfFile2 pfn_UnmapViewOfFile2{};

    std::mutex placeholder_mutex;                                 ///< Mutex for placeholders
    boost::icl::separate_interval_set<size_t> placeholders;       ///< Mapped placeholders
    ::Common::unordered_map<size_t, size_t> placeholder_host_pointers; ///< Placeholder backing offset
};

#elif defined(__OPENORBIS__) || defined(__managarm__)
// None of the luxuries of POSIX, all of the suffering
// For managarm: see https://github.com/managarm/managarm/issues/1370
#else // ^^^ Windows ^^^ vvv POSIX vvv

#ifndef MAP_NOCORE
#define MAP_NOCORE 0
#endif

#ifdef ARCHITECTURE_arm64

static void* ChooseVirtualBase(size_t virtual_size) {
    constexpr uintptr_t Map39BitSize = (1ULL << 39);
    constexpr uintptr_t Map36BitSize = (1ULL << 36);

    // This is not a cryptographic application, we just want something random.
    std::mt19937_64 rng;

    // We want to ensure we are allocating at an address aligned to the L2 block size.
    // For Qualcomm devices, we must also allocate memory above 36 bits.
    const size_t lower = Map36BitSize / HugePageSize;
    const size_t upper = (Map39BitSize - virtual_size) / HugePageSize;
    const size_t range = upper - lower;

    // Try up to 64 times to allocate memory at random addresses in the range.
    for (int i = 0; i < 64; i++) {
        // Calculate a possible location.
        uintptr_t hint_address = ((rng() % range) + lower) * HugePageSize;

        // Try to map.
        // Note: we may be able to take advantage of MAP_FIXED_NOREPLACE here.
        void* map_pointer =
            mmap(reinterpret_cast<void*>(hint_address), virtual_size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_NOCORE, -1, 0);

        // If we successfully mapped, we're done.
        if (reinterpret_cast<uintptr_t>(map_pointer) == hint_address) {
            return map_pointer;
        }

        // Unmap if necessary, and try again.
        if (map_pointer != MAP_FAILED) {
            munmap(map_pointer, virtual_size);
        }
    }

    return MAP_FAILED;
}

#else

static void* ChooseVirtualBase(size_t virtual_size) {
#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__) || defined(__sun__) || defined(__HAIKU__) || defined(__managarm__) || defined(__AIX__)
    void* virtual_base = mmap(nullptr, virtual_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_ALIGNED_SUPER | MAP_NOCORE, -1, 0);
    if (virtual_base != MAP_FAILED)
        return virtual_base;
#endif
    return mmap(nullptr, virtual_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_NOCORE, -1, 0);
}

#endif

#if defined(__sun__) || defined(__HAIKU__) || defined(__NetBSD__) || defined(__DragonFly__)
/// Most Unices don't have a portable shm_open (AIX, OpenBSD, NetBSD, Solaris 11, OpenIndiana)
/// Portable implementation of shm_open(SHM_ANON, ...) - roughly equivalent but without
/// OS support - may fail sporadically, beware!
static int shm_open_anon(int flags, mode_t mode) {
    char name[16] = "/shm-";
    char *const limit = name + sizeof(name) - 1;
    *limit = '\0';
    char *start = name + strlen(name);
    for (int tries = 0; tries < 4; tries++) {
        struct timespec tv;
        clock_gettime(CLOCK_REALTIME, &tv);
        unsigned long r = (unsigned long)tv.tv_sec + (unsigned long)tv.tv_nsec;
        for (char *fill = start; fill < limit; r /= 8)
            *fill++ = '0' + (r % 8);
        int fd = shm_open(name, flags, mode);
        if (fd != -1) {
            if (shm_unlink(name) == -1) {
                int tmp = errno;
                close(fd);
                errno = tmp;
                return -1;
            }
            return fd;
        }
        if (errno != EEXIST)
            break;
    }
    return -1;
}
#elif defined(__OpenBSD__)
/// Except OpenBSD which explicitly uses shm_mkstemp instead (as a more secure alternative)
static int shm_open_anon(int flags, mode_t mode) {
    char name[16] = "/shm-XXXXXXXXXX";
    int fd;
    if ((fd = shm_mkstemp(name)) == -1)
        return -1;
    if (shm_unlink(name) == -1) {
        int tmp = errno;
        close(fd);
        errno = tmp;
        return -1;
    }
    return fd;
}
#endif

class HostMemory::Impl {
public:
    explicit Impl(size_t backing_size_, size_t virtual_size_)
        : backing_size{backing_size_}
        , virtual_size{virtual_size_}
    {}

    bool Init() {
        long page_size = sysconf(_SC_PAGESIZE);
        ASSERT_MSG(page_size == 0x1000, "page size {:#x} is incompatible with 4K paging", page_size);
        // Backing memory initialization
#if defined(__sun__) || defined(__HAIKU__) || defined(__NetBSD__) || defined(__DragonFly__)
        fd = shm_open_anon(O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
#elif defined(__OpenBSD__)
        fd = shm_open_anon(O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
#elif defined(__FreeBSD__)
        fd = shm_open(SHM_ANON, O_RDWR, 0600);
#elif defined(__APPLE__) || defined(__managarm__)
        // macOS doesn't have memfd_create, use anonymous temporary file
        char template_path[] = "/tmp/eden_mem_XXXXXX";
        fd = mkstemp(template_path);
        if (fd >= 0) {
            unlink(template_path);
        }
#else
        fd = memfd_create("HostMemory", 0);
#endif
        bool use_anon = false;
        if (fd <= 0) {
            LOG_WARNING(Common_Memory, "memfd_create: {}", strerror(errno));
            use_anon = true;
        }
        if (!use_anon) {
            // Defined to extend the file with zeros
            int ret = ftruncate(fd, backing_size);
            if (ret != 0) {
                LOG_WARNING(Common_Memory, "ftruncate: {} (likely out-of-emory)", strerror(errno));
                use_anon = true;
            }
        }
        if (use_anon) {
            LOG_WARNING(Common_Memory, "Using private mappings instead of shared ones");
            backing_base = static_cast<u8*>(mmap(nullptr, backing_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_NOCORE, -1, 0));
            if (fd > 0) {
                fd = -1;
                close(fd);
            }
        } else {
            backing_base = static_cast<u8*>(mmap(nullptr, backing_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NOCORE, fd, 0));
        }
        if (backing_base == MAP_FAILED) {
            LOG_CRITICAL(HW_Memory, "mmap failed: {}", strerror(errno));
            return false;
        }

        // Virtual memory initialization
        virtual_base = virtual_map_base = static_cast<u8*>(ChooseVirtualBase(virtual_size));
        if (virtual_base == MAP_FAILED) {
            LOG_CRITICAL(HW_Memory, "mmap failed: {}", strerror(errno));
            return false;
        }
#if defined(__linux__)
        madvise(virtual_base, virtual_size, MADV_HUGEPAGE);
#endif
        free_manager.SetAddressSpace(virtual_base, virtual_size);
        return true;
    }

    ~Impl() {
        Release();
    }

    void* Allocate(size_t size) {
        auto* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (ptr == MAP_FAILED) {
            LOG_CRITICAL(HW_Memory, "Failed to allocate fallback buffer with size {:#x}, {}", size, strerror(errno));
        }
        return ptr;
    }

    void Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms) {
        // Intersect the range with our address space.
        AdjustMap(&virtual_offset, &length);

        // We are removing a placeholder.
        free_manager.AllocateBlock(virtual_base + virtual_offset, length);

        // Deduce mapping protection flags.
        int prot_flags = PROT_NONE;
        if (True(perms & MemoryPermission::Read))
            prot_flags |= PROT_READ;
        if (True(perms & MemoryPermission::Write))
            prot_flags |= PROT_WRITE;
#ifdef ARCHITECTURE_arm64
        if (True(perms & MemoryPermission::Execute))
            prot_flags |= PROT_EXEC;
#endif
        int flags = (fd >= 0 ? MAP_SHARED : MAP_PRIVATE) | MAP_FIXED;
        void* ret = mmap(virtual_base + virtual_offset, length, prot_flags, flags, fd, host_offset);
        ASSERT_MSG(ret != MAP_FAILED, "mmap: {} {}", strerror(errno), fd);
    }

    void Unmap(size_t virtual_offset, size_t length) {
        // The method name is wrong. We're still talking about the virtual range.
        // We don't want to unmap, we want to reserve this memory.

        // Intersect the range with our address space.
        AdjustMap(&virtual_offset, &length);

        // Merge with any adjacent placeholder mappings.
        auto [merged_pointer, merged_size] =
            free_manager.FreeBlock(virtual_base + virtual_offset, length);

        void* ret = mmap(merged_pointer, merged_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        ASSERT_MSG(ret != MAP_FAILED, "mmap: {}", strerror(errno));
    }

    void Protect(size_t virtual_offset, size_t length, bool read, bool write, bool execute) {
        // Intersect the range with our address space.
        AdjustMap(&virtual_offset, &length);

        int flags = PROT_NONE;
        if (read) {
            flags |= PROT_READ;
        }
        if (write) {
            flags |= PROT_WRITE;
        }
#ifdef HAS_NCE
        if (execute) {
            flags |= PROT_EXEC;
        }
#endif
        int ret = mprotect(virtual_base + virtual_offset, length, flags);
        ASSERT_MSG(ret == 0, "mprotect failed: {}", strerror(errno));
    }

    void EnableDirectMappedAddress() {
        virtual_base = nullptr;
    }

    const size_t backing_size; ///< Size of the backing memory in bytes
    const size_t virtual_size; ///< Size of the virtual address placeholder in bytes

    u8* backing_base{reinterpret_cast<u8*>(MAP_FAILED)};
    u8* virtual_base{reinterpret_cast<u8*>(MAP_FAILED)};
    u8* virtual_map_base{reinterpret_cast<u8*>(MAP_FAILED)};

private:
    /// Release all resources in the object
    void Release() {
        if (virtual_map_base != MAP_FAILED) {
            int ret = munmap(virtual_map_base, virtual_size);
            ASSERT_MSG(ret == 0, "munmap failed: {}", strerror(errno));
        }

        if (backing_base != MAP_FAILED) {
            int ret = munmap(backing_base, backing_size);
            ASSERT_MSG(ret == 0, "munmap failed: {}", strerror(errno));
        }

        if (fd != -1) {
            int ret = close(fd);
            ASSERT_MSG(ret == 0, "close failed: {}", strerror(errno));
        }
    }

    void AdjustMap(size_t* virtual_offset, size_t* length) {
        if (virtual_base != nullptr) {
            return;
        }

        // If we are direct mapped, we want to make sure we are operating on a region
        // that is in range of our virtual mapping.
        size_t intended_start = *virtual_offset;
        size_t intended_end = intended_start + *length;
        size_t address_space_start = reinterpret_cast<size_t>(virtual_map_base);
        size_t address_space_end = address_space_start + virtual_size;

        if (address_space_start > intended_end || intended_start > address_space_end) {
            *virtual_offset = 0;
            *length = 0;
        } else {
            *virtual_offset = (std::max)(intended_start, address_space_start);
            *length = (std::min)(intended_end, address_space_end) - *virtual_offset;
        }
    }

    int fd{-1}; // memfd file descriptor, -1 is the error value of memfd_create
    FreeRegionManager free_manager{};
};

#endif // ^^^ POSIX ^^^

#ifdef __OPENORBIS__
extern "C" __attribute__((weak)) void ps4_boot_trace(const char* stage); // externals/ps4sup/crashlog.cpp
extern "C" int32_t sceKernelEnableDmemAliasing(); // libkernel; not in the OpenOrbis headers

/// Emulated DRAM and fastmem on the PS4.
///
/// Backing: direct memory (the ~4.5 GiB pool the GPU also uses); anonymous mappings top out
/// around 1 GiB and only ~384 MiB of flexible memory is free (seen on hardware). The 4 GiB backing
/// range is only reserved: 2 MiB chunks of direct memory are allocated and mapped into it the
/// first time something touches them (ps4_dram_fault, called first by dynarmic's and the swap
/// fault handlers) or a fastmem mapping needs them. Backing all 4 GiB up front left ~500 MiB for
/// RADV and ran it out of memory in deko3d's examples.
///
/// Fastmem: the guest address space is a reserved virtual range; guest mappings become direct
/// memory mappings of the backing into it (direct memory aliasing). The PS4 maps memory in 16 KiB
/// pages, guest pages are 4 KiB: a 16 KiB block is mapped only when its four guest pages map four
/// consecutive, 16 KiB aligned backing pages with the same permissions - the usual case. Any other
/// block stays unmapped; dynarmic's fault handler then sends those accesses down the slow path
/// (the page table), which handles them correctly. Read-only guest pages (rasterizer-cached
/// memory) are mapped read-only, so writes fault and take the slow path too.
class HostMemory::PS4Arena {
public:
    static constexpr size_t GuestPage = 0x1000, HostPage = 0x4000, Sub = HostPage / GuestPage;
    static constexpr int32_t ProtRead = 0x1, ProtReadWrite = 0x3, MapFixed = 0x10, WbOnion = 0;

    PS4Arena(size_t backing_size, size_t virtual_size) {
        constexpr size_t Align = 0x200000;
        backing_size = (backing_size + Align - 1) & ~(Align - 1);
        const size_t total = sceKernelGetDirectMemorySize();
        off_t largest_at = 0;
        size_t largest = 0;
        // <orbis/libkernel.h> declares the two outputs as values; they are pointers.
        using AvailableFn = int32_t (*)(off_t, off_t, size_t, off_t*, size_t*);
        reinterpret_cast<AvailableFn>(&sceKernelAvailableDirectMemorySize)(0, off_t(total), Align, &largest_at, &largest);
        Trace("host memory: %zu MiB emulated DRAM; direct memory %zu MiB total, largest free %zu MiB",
              backing_size >> 20, total >> 20, largest >> 20);

        // Mapping the same direct memory twice (backing + fastmem view) needs aliasing enabled.
        const int32_t alias = sceKernelEnableDmemAliasing();
        // Above where libkernel places its own objects (DolphinPS4: 0x200000000 collided).
        void* addr = reinterpret_cast<void*>(0x2000000000ULL);
        int32_t result = sceKernelReserveVirtualRange(&addr, backing_size, MapFixed, Align);
        Trace("host memory: backing range %s (%#x) at %p, committed in %zu KiB chunks on use, aliasing %#x",
              result == 0 ? "reserved" : "FAILED", unsigned(result), addr, Chunk >> 10, unsigned(alias));
        if (result != 0) {
            ASSERT_MSG(false, "Could not reserve {} MiB for emulated DRAM", backing_size >> 20);
            return;
        }
        backing_base = static_cast<u8*>(addr);
        backing_bytes = backing_size;
        chunk_phys.assign(backing_size / Chunk, -1);
        g_dram.store(this);
        MeasureBandwidth();

        struct stat st;
        if (stat("/data/switch-emu/no-fastmem", &st) == 0) {
            Trace("fastmem: off (/data/switch-emu/no-fastmem)");
            return;
        }
        if (alias != 0) {
            Trace("fastmem: off (no direct memory aliasing)");
            return;
        }
        // The whole 39-bit guest space (512 GiB) is refused (0x8002000c on hardware): take the
        // largest power of two the kernel gives. dynarmic then bounds-checks against it and sends
        // accesses above it down the slow path (ArmDynarmic64/32 set fastmem_address_space_bits).
        for (size_t size = virtual_size; size >= (size_t(8) << 30); size /= 2) {
            void* reserved = nullptr;
            result = sceKernelReserveVirtualRange(&reserved, size, 0, Align);
            if (result == 0) {
                virtual_base = static_cast<u8*>(reserved);
                arena_size = size;
                reserved_bytes = size;
                break;
            }
        }
        // One call tops out at 16 GiB, but reservations right after it may work: grow the arena
        // while they do (the JIT's window is the largest power of two that fits), so more of
        // the guest space (the kernel map region starts at 12 GiB) gets fastmem.
        if (virtual_base) {
            const size_t step = arena_size;
            while (reserved_bytes + step <= virtual_size) {
                // A hint, not MAP_FIXED: a fixed reservation would replace whatever is mapped there.
                void* const want = virtual_base + reserved_bytes;
                void* next = want;
                if (sceKernelReserveVirtualRange(&next, step, 0, Align) != 0)
                    break;
                if (next != want) { // placed elsewhere: give it back, stop growing
                    sceKernelMunmap(next, step);
                    break;
                }
                reserved_bytes += step;
            }
            while (arena_size * 2 <= reserved_bytes)
                arena_size *= 2;
        }
        Trace("fastmem: %s %zu GiB arena at %p (%zu GiB reserved, guest space %zu GiB)",
              virtual_base ? "reserved" : "could not reserve", arena_size >> 30, static_cast<void*>(virtual_base),
              reserved_bytes >> 30, virtual_size >> 30);
    }

    ~PS4Arena() {
        // Eden re-creates DeviceMemory when core settings change (e.g. multi-core): give the
        // direct memory back, or the next 4 GiB allocation fails.
        if (virtual_base)
            sceKernelMunmap(virtual_base, reserved_bytes);
        if (backing_base) {
            g_dram.store(nullptr);
            sceKernelMunmap(backing_base, backing_bytes);
            for (const off_t chunk : chunk_phys)
                if (chunk >= 0)
                    sceKernelReleaseDirectMemory(chunk, Chunk);
        }
    }

    /// ClearBackingRegion: zero-filling a chunk nobody has touched yet is a no-op (it's zeroed
    /// when committed). The kernel clears every heap it hands out - libnx homebrew takes nearly
    /// all free memory as heap - which otherwise committed ~3 GiB at once on hardware.
    void Clear(size_t offset, size_t length, u32 fill_value) {
        while (length != 0) {
            const size_t chunk = offset / Chunk;
            const size_t n = std::min(length, (chunk + 1) * Chunk - offset);
            bool skip = false;
            if (fill_value == 0) {
                std::scoped_lock lk{commit_mutex};
                skip = chunk_phys[chunk] < 0;
            }
            if (!skip)
                std::memset(backing_base + offset, int(fill_value), n);
            offset += n;
            length -= n;
        }
    }

    /// Fault on the backing range: commit the chunk and retry. False for any other address.
    bool HandleFault(uintptr_t address) {
        const uintptr_t offset = address - uintptr_t(backing_base);
        if (!backing_base || offset >= backing_bytes)
            return false;
        return Commit(offset / Chunk) >= 0;
    }

    u8* backing_base{};
    u8* virtual_base{};

    void Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms) {
        std::scoped_lock lk{mutex};
        if (logged_maps < 24) {
            logged_maps++;
            Trace("fastmem: map guest %#zx -> backing %#zx, %zu KiB, perm %u (%s)", virtual_offset, host_offset,
                  length >> 10, unsigned(perms),
                  ((virtual_offset ^ host_offset) & (HostPage - 1)) == 0 ? "16K-congruent" : "NOT congruent");
        }
        if (!Clip(virtual_offset, length))
            return;
        for (size_t i = 0; i < length; i += GuestPage) {
            Block& b = blocks[(virtual_offset + i) / HostPage];
            const size_t sub = ((virtual_offset + i) / GuestPage) % Sub;
            b.page[sub] = u32((host_offset + i) / GuestPage) + 1;
            b.perm[sub] = u8(perms);
        }
        Update(virtual_offset, length);
    }

    void Unmap(size_t virtual_offset, size_t length) {
        std::scoped_lock lk{mutex};
        if (!Clip(virtual_offset, length))
            return;
        for (size_t i = 0; i < length; i += GuestPage) {
            const auto it = blocks.find((virtual_offset + i) / HostPage);
            if (it != blocks.end())
                it->second.page[((virtual_offset + i) / GuestPage) % Sub] = 0;
        }
        Update(virtual_offset, length);
    }

    void Protect(size_t virtual_offset, size_t length, MemoryPermission perms) {
        std::scoped_lock lk{mutex};
        if (!Clip(virtual_offset, length))
            return;
        for (size_t i = 0; i < length; i += GuestPage) {
            const auto it = blocks.find((virtual_offset + i) / HostPage);
            if (it != blocks.end())
                it->second.perm[((virtual_offset + i) / GuestPage) % Sub] = u8(perms);
        }
        Update(virtual_offset, length);
    }

    size_t ArenaSize() const {
        return arena_size;
    }

private:
    /// Limits a guest range to the arena; false when nothing of it is inside.
    bool Clip(size_t offset, size_t& length) const {
        if (offset >= arena_size)
            return false;
        length = std::min(length, arena_size - offset);
        return length != 0;
    }

    struct Block {
        u32 page[Sub]{};   ///< backing 4 KiB page index + 1 per guest page; 0 = unmapped
        u8 perm[Sub]{};    ///< MemoryPermission per guest page
        u32 applied_page{}; ///< first backing page index + 1 currently mapped here; 0 = none
        u8 applied_prot{};
    };

    /// What a block should be: the backing page index + 1 of its first guest page and the
    /// protection, or {0, 0} when it can't be mapped as one 16 KiB host page.
    static std::pair<u32, u8> Desired(const Block& b) {
        if (b.page[0] == 0 || (b.page[0] - 1) % Sub != 0)
            return {0, 0};
        const u8 perm = b.perm[0];
        for (size_t i = 1; i < Sub; i++) {
            if (b.page[i] != b.page[0] + i || b.perm[i] != perm)
                return {0, 0};
        }
        const bool read = (perm & u8(MemoryPermission::Read)) != 0;
        const bool write = (perm & u8(MemoryPermission::Write)) != 0;
        if (!read)
            return {0, 0};
        return {b.page[0], u8(write ? ProtReadWrite : ProtRead)};
    }

    /// Brings the host mappings of the blocks covering [offset, offset + length) in line with
    /// their guest pages. Consecutive blocks needing the same action become one kernel call.
    void Update(size_t offset, size_t length) {
        enum class Action { None, Map, Unmap, Protect };
        const u64 first = offset / HostPage, last = (offset + length - 1) / HostPage;
        Action run_action = Action::None;
        u64 run_start = 0, run_count = 0;
        u32 run_page = 0;
        u8 run_prot = 0;
        const auto flush = [&] {
            if (run_count == 0)
                return;
            void* addr = virtual_base + run_start * HostPage;
            const size_t size = run_count * HostPage;
            int32_t r = 0;
            switch (run_action) {
            case Action::Map: {
                // Replace whatever is there (the arena's reservation or an older mapping).
                sceKernelMunmap(addr, size);
                void* target = addr;
                const size_t host_offset = size_t(run_page - 1) * GuestPage;
                const off_t chunk = Commit(host_offset / Chunk);
                r = chunk < 0 ? -1
                              : sceKernelMapDirectMemory(&target, size, run_prot, MapFixed,
                                                         chunk + off_t(host_offset % Chunk), HostPage);
                break;
            }
            case Action::Unmap: {
                sceKernelMunmap(addr, size);
                // Keep the hole reserved so nothing else gets mapped inside the arena.
                void* hole = addr;
                r = sceKernelReserveVirtualRange(&hole, size, MapFixed, HostPage);
                break;
            }
            case Action::Protect:
                r = sceKernelMprotect(addr, size, run_prot);
                break;
            case Action::None:
                break;
            }
            if (r != 0 && failures++ < 16)
                Trace("fastmem: action %d on %zu KiB at %p failed (%#x)", int(run_action), size >> 10, addr, unsigned(r));
            calls++;
            run_count = 0;
        };
        for (u64 index = first; index <= last; index++) {
            const auto it = blocks.find(index);
            if (it == blocks.end()) {
                flush();
                continue;
            }
            Block& b = it->second;
            const auto [page, prot] = Desired(b);
            Action action = Action::None;
            if (page == 0 && b.applied_page != 0)
                action = Action::Unmap;
            else if (page != 0 && page != b.applied_page)
                action = Action::Map;
            else if (page != 0 && prot != b.applied_prot)
                action = Action::Protect;
            if (action == Action::None) {
                flush();
            } else {
                const bool extends = run_count != 0 && action == run_action && index == run_start + run_count &&
                                     (action == Action::Unmap ||
                                      (prot == run_prot && (action == Action::Protect ||
                                                            (page == run_page + run_count * Sub &&
                                                             ChunkOf(page) == ChunkOf(run_page)))));
                if (!extends) {
                    flush();
                    run_action = action;
                    run_start = index;
                    run_page = page;
                    run_prot = prot;
                }
                run_count++;
                if (b.applied_page == 0 && page != 0)
                    mapped_blocks++;
                else if (b.applied_page != 0 && page == 0)
                    mapped_blocks--;
                b.applied_page = page;
                b.applied_prot = prot;
            }
            if (b.applied_page == 0 && b.page[0] == 0 && b.page[1] == 0 && b.page[2] == 0 && b.page[3] == 0)
                blocks.erase(it);
        }
        flush();
        if (++updates % 4096 == 0)
            Trace("fastmem: %llu updates, %llu kernel calls, %llu blocks (%llu MiB) mapped, %zu tracked",
                  (unsigned long long)updates, (unsigned long long)calls, (unsigned long long)mapped_blocks,
                  (unsigned long long)(mapped_blocks * HostPage >> 20), blocks.size());
    }

public:
    /// Summary for the boot trace, with why tracked blocks aren't mapped.
    void Report(const char* when) {
        std::scoped_lock lk{mutex};
        size_t incomplete = 0, misaligned = 0, mixed = 0, no_read = 0;
        for (const auto& [index, b] : blocks) {
            if (b.applied_page != 0)
                continue;
            bool complete = true, contiguous = true, same = true;
            for (size_t i = 0; i < Sub; i++) {
                complete &= b.page[i] != 0;
                contiguous &= i == 0 || b.page[i] == b.page[0] + i;
                same &= b.perm[i] == b.perm[0];
            }
            if (!complete)
                incomplete++;
            else if (!contiguous || (b.page[0] - 1) % Sub != 0)
                misaligned++;
            else if (!same)
                mixed++;
            else
                no_read++;
        }
        Trace("fastmem (%s): %llu updates, %llu kernel calls, %llu blocks (%llu MiB) mapped, %zu tracked; "
              "unmapped: %zu incomplete, %zu misaligned, %zu mixed perms, %zu no read",
              when, (unsigned long long)updates, (unsigned long long)calls, (unsigned long long)mapped_blocks,
              (unsigned long long)(mapped_blocks * HostPage >> 20), blocks.size(), incomplete, misaligned, mixed,
              no_read);
    }

private:
    __attribute__((format(printf, 1, 2))) static void Trace(const char* fmt, ...) {
        char line[256];
        va_list args;
        va_start(args, fmt);
        vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);
        LOG_INFO(HW_Memory, "{}", line);
        if (ps4_boot_trace)
            ps4_boot_trace(line);
    }

    static constexpr size_t Chunk = 0x200000;

    /// MB/s copying `bytes` from src to dst a few times (both already mapped).
    static double CopySpeed(u8* dst, const u8* src, size_t bytes) {
        timespec t0, t1;
        std::memcpy(dst, src, bytes); // warm up / fault in
        clock_gettime(CLOCK_MONOTONIC, &t0);
        constexpr int kRounds = 8;
        for (int i = 0; i < kRounds; i++)
            std::memcpy(dst, src, bytes);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        const double seconds = double(t1.tv_sec - t0.tv_sec) + double(t1.tv_nsec - t0.tv_nsec) / 1e9;
        return seconds > 0 ? double(bytes) * kRounds / seconds / 1e6 : 0;
    }

    /// Guest memory copies ran ~60x slower than on a PC (vs ~5x for integer code). Times host
    /// copies in each kind of memory Eden uses, to see whether the emulated DRAM is the slow part:
    /// flexible memory (malloc), fresh direct memory, the DRAM backing, and a second (aliased)
    /// mapping of it as fastmem uses.
    void MeasureBandwidth() {
        constexpr size_t kBytes = 4u << 20;
        u8* flex = static_cast<u8*>(std::malloc(kBytes * 2));
        double flex_speed = 0, direct_speed = 0, backing_speed = 0, alias_speed = 0, mixed_speed = 0;
        if (flex)
            flex_speed = CopySpeed(flex + kBytes, flex, kBytes);
        off_t phys = 0;
        void* direct = nullptr;
        if (sceKernelAllocateDirectMemory(0, off_t(sceKernelGetDirectMemorySize()), kBytes * 2, Chunk, WbOnion, &phys) == 0) {
            if (sceKernelMapDirectMemory(&direct, kBytes * 2, ProtReadWrite, 0, phys, Chunk) == 0) {
                auto* d = static_cast<u8*>(direct);
                direct_speed = CopySpeed(d + kBytes, d, kBytes);
                sceKernelMunmap(direct, kBytes * 2);
            }
            sceKernelReleaseDirectMemory(phys, kBytes * 2);
        }
        // The backing's first chunks (committed now; the guest gets them anyway).
        const off_t c0 = Commit(0), c1 = Commit(1), c2 = Commit(2);
        if (c0 >= 0 && c1 >= 0 && c2 >= 0) {
            backing_speed = CopySpeed(backing_base + 2 * Chunk, backing_base, 2 * Chunk);
            if (flex)
                mixed_speed = CopySpeed(flex, backing_base, 2 * Chunk);
            void* alias = nullptr;
            if (sceKernelMapDirectMemory(&alias, Chunk, ProtReadWrite, 0, c0, Chunk) == 0) {
                alias_speed = CopySpeed(backing_base + 2 * Chunk, static_cast<u8*>(alias), Chunk);
                sceKernelMunmap(alias, Chunk);
            }
        }
        std::free(flex);
        Trace("host memory: copy speed MB/s - flexible %.0f, direct %.0f, DRAM backing %.0f, backing->flexible %.0f, "
              "aliased view %.0f",
              flex_speed, direct_speed, backing_speed, mixed_speed, alias_speed);
    }

    static size_t ChunkOf(u32 page) {
        return size_t(page - 1) * GuestPage / Chunk;
    }

    /// Direct memory offset of a backing chunk, allocating, mapping and zeroing it on first use;
    /// -1 when direct memory is exhausted. Safe in a signal handler: nothing here touches
    /// uncommitted backing, so the thread holding the lock never faults into it.
    off_t Commit(size_t chunk) {
        std::scoped_lock lk{commit_mutex};
        if (chunk_phys[chunk] >= 0)
            return chunk_phys[chunk];
        off_t phys = 0;
        int32_t r = sceKernelAllocateDirectMemory(0, off_t(sceKernelGetDirectMemorySize()), Chunk, Chunk,
                                                  WbOnion, &phys);
        void* addr = backing_base + chunk * Chunk;
        if (r == 0) {
            sceKernelMunmap(addr, Chunk); // the reservation
            void* target = addr;
            r = sceKernelMapDirectMemory(&target, Chunk, ProtReadWrite, MapFixed, phys, Chunk);
            if (r != 0) {
                sceKernelReleaseDirectMemory(phys, Chunk);
                void* hole = addr;
                sceKernelReserveVirtualRange(&hole, Chunk, MapFixed, Chunk);
            }
        }
        if (r != 0) {
            if (commit_failures++ < 8)
                Trace("host memory: committing DRAM chunk at %p failed (%#x), %zu MiB committed", addr, unsigned(r),
                      committed_chunks * Chunk >> 20);
            return -1;
        }
        std::memset(addr, 0, Chunk); // fresh DRAM reads as zero, as with Eden's memfd backing
        chunk_phys[chunk] = phys;
        if (++committed_chunks % 128 == 0)
            Trace("host memory: %zu MiB of emulated DRAM committed", committed_chunks * Chunk >> 20);
        return phys;
    }

public:
    static inline std::atomic<PS4Arena*> g_dram{};

private:
    std::mutex commit_mutex;
    std::vector<off_t> chunk_phys; ///< direct memory offset per backing chunk; -1 = not committed
    size_t committed_chunks{};
    unsigned commit_failures{};
    size_t reserved_bytes{}; ///< fastmem reservation (arena_size rounded down to a power of two)
    size_t backing_bytes{};
    size_t arena_size{};
    std::mutex mutex;
    std::unordered_map<u64, Block> blocks;
    u64 updates{}, calls{}, mapped_blocks{}, failures{};
    int logged_maps{};
};

/// Called first by the SIGSEGV/SIGBUS handlers (dynarmic's, sparse_large_vector's swap handler):
/// true when the fault was on uncommitted emulated DRAM, now committed - retry the access.
extern "C" bool ps4_dram_fault(uintptr_t address) {
    HostMemory::PS4Arena* const arena = HostMemory::PS4Arena::g_dram.load();
    return arena && arena->HandleFault(address);
}
#endif

HostMemory::HostMemory(size_t backing_size_, size_t virtual_size_)
    : backing_size(backing_size_)
    , virtual_size(virtual_size_)
{
#if defined(__OPENORBIS__)
    ps4_arena = std::make_unique<PS4Arena>(backing_size, virtual_size);
    backing_base = ps4_arena->backing_base;
    virtual_base = ps4_arena->virtual_base;
#elif defined(__managarm__)
    LOG_WARNING(HW_Memory, "Platform doesn't support fastmem");
    backing_base = static_cast<u8*>(mmap(nullptr, backing_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    virtual_base = nullptr;
#else
    // Try to allocate a fastmem arena.
    impl = std::make_unique<HostMemory::Impl>(AlignUp(backing_size, PageAlignment), AlignUp(virtual_size, PageAlignment) + HugePageSize);
    if (impl->Init()) {
        backing_base = impl->backing_base;
        virtual_base = impl->virtual_base;
        if (virtual_base) {
            // Ensure the virtual base is aligned to the L2 block size.
            virtual_base = reinterpret_cast<u8*>(Common::AlignUp(uintptr_t(virtual_base), HugePageSize));
            virtual_base_offset = virtual_base - impl->virtual_base;
        }
    } else {
        LOG_WARNING(HW_Memory, "Platform can support fastmem, but can't create it");
        fallback_buffer = true;
        backing_base = static_cast<u8*>(impl->Allocate(backing_size));
        virtual_base = nullptr;
        impl.reset();
    }
#endif
}

HostMemory::~HostMemory() {
#ifdef _WIN32
    if (fallback_buffer) {
        VirtualFree(backing_base, backing_size, MEM_RELEASE);
    }
#else
    if (fallback_buffer) {
        munmap(backing_base, backing_size);
    }
#endif
}

HostMemory::HostMemory(HostMemory&&) noexcept = default;

HostMemory& HostMemory::operator=(HostMemory&&) noexcept = default;

void HostMemory::Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms, bool separate_heap) {
#if defined(__OPENORBIS__)
    if (ps4_arena && virtual_base && length != 0)
        ps4_arena->Map(virtual_offset, host_offset, length, perms);
#elif !defined(__managarm__)
    ASSERT(virtual_offset % PageAlignment == 0);
    ASSERT(host_offset % PageAlignment == 0);
    ASSERT(length % PageAlignment == 0);
    ASSERT(virtual_offset + length <= virtual_size);
    ASSERT(host_offset + length <= backing_size);
    if (length == 0 || !virtual_base || !impl) {
        return;
    }
    impl->Map(virtual_offset + virtual_base_offset, host_offset, length, perms);
#endif
}

void HostMemory::Unmap(size_t virtual_offset, size_t length, bool separate_heap) {
#if defined(__OPENORBIS__)
    if (ps4_arena && virtual_base && length != 0)
        ps4_arena->Unmap(virtual_offset, length);
#elif !defined(__managarm__)
    ASSERT(virtual_offset % PageAlignment == 0);
    ASSERT(length % PageAlignment == 0);
    ASSERT(virtual_offset + length <= virtual_size);
    if (length == 0 || !virtual_base || !impl) {
        return;
    }
    impl->Unmap(virtual_offset + virtual_base_offset, length);
#endif
}

void HostMemory::Protect(size_t virtual_offset, size_t length, MemoryPermission perm) {
#if defined(__OPENORBIS__)
    if (ps4_arena && virtual_base && length != 0)
        ps4_arena->Protect(virtual_offset, length, perm);
#elif !defined(__managarm__)
    ASSERT(virtual_offset % PageAlignment == 0);
    ASSERT(length % PageAlignment == 0);
    ASSERT(virtual_offset + length <= virtual_size);
    if (length == 0 || !virtual_base || !impl) {
        return;
    }
    const bool read = True(perm & MemoryPermission::Read);
    const bool write = True(perm & MemoryPermission::Write);
    const bool execute = True(perm & MemoryPermission::Execute);
    impl->Protect(virtual_offset + virtual_base_offset, length, read, write, execute);
#endif
}

size_t HostMemory::FastmemArenaSize() const noexcept {
#ifdef __OPENORBIS__
    return ps4_arena && virtual_base ? ps4_arena->ArenaSize() : 0;
#else
    return virtual_base ? virtual_size : 0;
#endif
}

void HostMemory::LogFastmemStats([[maybe_unused]] const char* when) {
#ifdef __OPENORBIS__
    if (ps4_arena && virtual_base)
        ps4_arena->Report(when);
#endif
}

void HostMemory::ClearBackingRegion(size_t physical_offset, size_t length, u32 fill_value) {
#ifdef __OPENORBIS__
    if (ps4_arena) {
        ps4_arena->Clear(physical_offset, length, fill_value);
        return;
    }
#endif
    std::memset(backing_base + physical_offset, fill_value, length);
}

void HostMemory::EnableDirectMappedAddress() {
#if !(defined(__OPENORBIS__) || defined(__managarm__))
    if (impl) {
        impl->EnableDirectMappedAddress();
        virtual_size += reinterpret_cast<uintptr_t>(virtual_base);
    }
#endif
}

} // namespace Common
