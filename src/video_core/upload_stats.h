// SPDX-FileCopyrightText: Copyright 2026 PS4 Switch Emu
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

namespace VideoCommon {

// Bytes moved from guest memory to the GPU, for the PS4's perf trace (and EDEN_UPLOAD_STATS=1 on a
// PC): how much work Eden's copies cost before a zero-copy path for unified memory is worth it.
struct UploadStats {
    std::atomic<u64> buffer_bytes{0};      // buffer cache: guest memory -> staging -> GPU buffer
    std::atomic<u64> inline_bytes{0};      // inline data (the guest's GPU commands) into buffers
    std::atomic<u64> uniform_bytes{0};     // per-draw constant buffers copied into the stream buffer
    std::atomic<u64> direct_bytes{0};      // of buffer + inline: written in place (no staging copy)
    std::atomic<u64> texture_cpu_bytes{0}; // textures unswizzled on the CPU
    std::atomic<u64> texture_converted_bytes{0}; // of those, also converted on the CPU (ASTC etc.)
    std::atomic<u64> texture_gpu_bytes{0}; // textures unswizzled/decoded by the GPU
    std::atomic<u64> texture_count{0};
};

inline UploadStats upload_stats;

} // namespace VideoCommon
