// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef __OPENORBIS__

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include <sys/mman.h>

#include "xbyak/xbyak.h"

namespace Dynarmic::Common {

/// Xbyak allocator for the PS4: maps JIT code memory as read/write/execute in one mmap call and
/// never mprotects it. Jailbreaks differ in how a homebrew app may get executable memory; the
/// PS4 runtime (externals/ps4sup, from DolphinPS4) probes the working method at startup and its
/// mmap wrapper applies it to every executable mapping - but only when PROT_EXEC is asked for at
/// mmap time, which Xbyak's default allocator (map RW, then mprotect) never does.
class PS4ExecAllocator final : public Xbyak::Allocator {
public:
    uint8_t* alloc(size_t size) override {
        const size_t page = Xbyak::inner::getPageSize();
        size = (size + page - 1) & ~(page - 1);
        void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED)
            return nullptr; // Xbyak's CodeArray reports ERR_CANT_ALLOC
        std::scoped_lock lk{mutex};
        sizes[p] = size;
        return static_cast<uint8_t*>(p);
    }
    void free(uint8_t* p) override {
        if (!p)
            return;
        std::scoped_lock lk{mutex};
        if (auto it = sizes.find(p); it != sizes.end()) {
            munmap(p, it->second);
            sizes.erase(it);
        }
    }
    bool useProtect() const override {
        return false;
    }

    static PS4ExecAllocator& Get() {
        static PS4ExecAllocator instance;
        return instance;
    }

private:
    std::mutex mutex;
    std::unordered_map<void*, size_t> sizes;
};

} // namespace Dynarmic::Common

#endif
