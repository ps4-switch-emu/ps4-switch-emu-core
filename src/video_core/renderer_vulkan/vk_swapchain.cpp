// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <mutex>
#include <thread>
#include <cstdarg>
#include <cstdio>
#include <array>
#include <chrono>
#include <limits>
#include <vector>

#ifdef __ANDROID__
#include <android/api-level.h>
#endif

#include "common/alignment.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/vulkan_common/vk_enum_string_helper.h"
#include "video_core/vulkan_common/vulkan_device.h"

#ifdef __OPENORBIS__
#include <atomic>
#include <unistd.h>
extern "C" {
int32_t sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void* param);
int32_t sceVideoOutRegisterBuffers(int32_t handle, int32_t start, void* const* addresses, int32_t count,
                                   const void* attribute);
int32_t sceVideoOutUnregisterBuffers(int32_t handle, int32_t set);
int32_t sceVideoOutSubmitFlip(int32_t handle, int32_t index, uint32_t mode, int64_t arg);
int32_t sceVideoOutGetFlipStatus(int32_t handle, void* status);
int32_t sceVideoOutSetFlipRate(int32_t handle, int32_t rate);
// Mesa PS4 winsys (DolphinPS4 port/mesa/ac_ps4_drm.c): queues a VideoOut flip behind the GPU work
// submitted so far.
int ac_ps4_submit_flip(int video, unsigned buffer, unsigned mode, int64_t arg);
__attribute__((weak)) void ps4_boot_trace(const char* stage);
// externals/ps4sup/status_screen.cpp: draws the loading screen into a 1920x1080 framebuffer.
__attribute__((weak)) void ps4_status_render_loading(uint32_t* dst, int pitch, const char* progress);
void ps4_swapchain_show_loading(const char* progress); // below
}
#endif
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "vulkan/vulkan_core.h"

#ifdef __OPENORBIS__
// Lag spike diagnostics, defined in externals/ps4sup/stub.cpp.
extern std::atomic<unsigned long long> ps4_fastmem_faults, ps4_pipelines_built;
#endif

namespace Vulkan {

#ifdef __OPENORBIS__
namespace {
constexpr u32 PS4Images = 3, PS4Width = 1920, PS4Height = 1080;
constexpr VkDeviceSize PS4FramebufferAlign = 2ULL << 20; // VideoOut buffer alignment
constexpr uint32_t PS4FlipVsync = 1;
int g_ps4_video = -1; // one VideoOut handle for the process
// Loading screen: drawn into swap image 0 and flipped until the game presents its first frame.
uint32_t* g_ps4_loading_dst = nullptr;
u32 g_ps4_loading_pitch = 0;
std::atomic<bool> g_ps4_presented{false};
// Redraws the loading screen (~15 fps: its spinner, elapsed time, shader count) from the swap
// chain's creation until the game acquires its first image; the lock keeps a redraw and the
// game's first frame apart (both use image 0).
std::mutex g_ps4_loading_mutex;
std::thread g_ps4_loading_thread;
bool g_ps4_loading_stop = false;

void StopLoadingScreenPS4() {
    {
        std::scoped_lock lk{g_ps4_loading_mutex};
        g_ps4_loading_stop = true;
    }
    if (g_ps4_loading_thread.joinable())
        g_ps4_loading_thread.join();
}

void StartLoadingScreenPS4() {
    if (g_ps4_loading_thread.joinable())
        return;
    g_ps4_loading_stop = false;
    g_ps4_loading_thread = std::thread([] {
        for (;;) {
            {
                std::scoped_lock lk{g_ps4_loading_mutex};
                if (g_ps4_loading_stop || g_ps4_presented || !g_ps4_loading_dst || !ps4_status_render_loading)
                    return;
                ps4_status_render_loading(g_ps4_loading_dst, int(g_ps4_loading_pitch), nullptr);
                sceVideoOutSubmitFlip(g_ps4_video, 0, PS4FlipVsync, 0);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(66));
        }
    });
}
// The swap chain in use and its image addresses (one per process), for the Home menu overlay.
Swapchain* g_ps4_swapchain = nullptr;
std::array<uint32_t*, 3> g_ps4_addresses{};
std::atomic<u64> g_ps4_gpu_wait_us{0};

struct PS4FlipStatus { // OrbisVideoOutFlipStatus
    u64 count, process_time, tsc;
    s64 flip_arg;
    u64 submit_tsc, reserved0;
    s32 gc_queue_num, flip_pending_num, current_buffer;
    u32 reserved1;
};

void PS4Trace(const char* fmt, ...) {
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    LOG_INFO(Render_Vulkan, "{}", line);
    if (ps4_boot_trace)
        ps4_boot_trace(line);
}
} // namespace
#endif

namespace {

VkSurfaceFormatKHR ChooseSwapSurfaceFormat(vk::Span<VkSurfaceFormatKHR> formats) {
    if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
        VkSurfaceFormatKHR format;
        format.format = VK_FORMAT_B8G8R8A8_UNORM;
        format.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        return format;
    }
    const auto& found = std::find_if(formats.begin(), formats.end(), [](const auto& format) {
        return format.format == VK_FORMAT_B8G8R8A8_UNORM &&
               format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    return found != formats.end() ? *found : formats[0];
}

static VkPresentModeKHR ChooseSwapPresentMode(bool has_imm, bool has_mailbox,
                                              bool has_fifo_relaxed) {
    // Mailbox doesn't lock the application like FIFO (vsync)
    // FIFO present mode locks the framerate to the monitor's refresh rate
    Settings::VSyncMode setting = [has_imm, has_mailbox]() {
        // Choose Mailbox or Immediate if unlocked and those modes are supported
        const auto mode = Settings::values.vsync_mode.GetValue();
        if (Settings::values.frame_gen.GetValue()) {
            return mode == Settings::VSyncMode::FifoRelaxed ? mode : Settings::VSyncMode::Fifo;
        }
        if (Settings::values.use_speed_limit.GetValue() &&
            Settings::values.current_speed_mode.GetValue() != Settings::SpeedMode::Turbo) {
            return mode;
        }
        switch (mode) {
        case Settings::VSyncMode::Fifo:
        case Settings::VSyncMode::FifoRelaxed:
            if (has_mailbox) {
                return Settings::VSyncMode::Mailbox;
            } else if (has_imm) {
                return Settings::VSyncMode::Immediate;
            }
            [[fallthrough]];
        default:
            return mode;
        }
    }();
    if (setting == Settings::VSyncMode::Immediate && !has_imm) {
        setting = Settings::VSyncMode::Mailbox;
    }
    if ((setting == Settings::VSyncMode::Mailbox && !has_mailbox) ||
        (setting == Settings::VSyncMode::FifoRelaxed && !has_fifo_relaxed)) {
        setting = Settings::VSyncMode::Fifo;
    }

    switch (setting) {
    case Settings::VSyncMode::Immediate:
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    case Settings::VSyncMode::Mailbox:
        return VK_PRESENT_MODE_MAILBOX_KHR;
    case Settings::VSyncMode::Fifo:
        return VK_PRESENT_MODE_FIFO_KHR;
    case Settings::VSyncMode::FifoRelaxed:
        return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    default:
        return VK_PRESENT_MODE_FIFO_KHR;
    }
}

VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, u32 width, u32 height) {
    constexpr auto undefined_size{(std::numeric_limits<u32>::max)()};
    if (capabilities.currentExtent.width != undefined_size) {
        return capabilities.currentExtent;
    }
    VkExtent2D extent;
    extent.width = (std::max)(capabilities.minImageExtent.width,
                            (std::min)(capabilities.maxImageExtent.width, width));
    extent.height = (std::max)(capabilities.minImageExtent.height,
                             (std::min)(capabilities.maxImageExtent.height, height));
    return extent;
}

VkCompositeAlphaFlagBitsKHR ChooseAlphaFlags(const VkSurfaceCapabilitiesKHR& capabilities) {
    if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) {
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    } else if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
        return VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    } else {
        LOG_ERROR(Render_Vulkan, "Unknown composite alpha flags value {:#x}",
                  capabilities.supportedCompositeAlpha);
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }
}

} // Anonymous namespace

Swapchain::Swapchain(
    VkSurfaceKHR_T* surface_,
    const Device& device_,
    Scheduler& scheduler_,
    u32 width_,
    u32 height_)
    : surface(surface_)
    , device{device_}
    , scheduler{scheduler_}
{
    Create(surface, width_, height_);
}

Swapchain::~Swapchain() = default;

void Swapchain::Create(
    VkSurfaceKHR_T* surface_,
    u32 width_,
    u32 height_)
{
    is_outdated = false;
    is_suboptimal = false;
    width = width_;
    height = height_;
    surface = surface_;

#ifdef __OPENORBIS__
    Destroy();
    CreatePS4();
    CreateSemaphores();
    resource_ticks.clear();
    resource_ticks.resize(image_count);
    return;
#endif

    const auto physical_device = device.GetPhysical();
    const auto capabilities{physical_device.GetSurfaceCapabilitiesKHR(VkSurfaceKHR(surface))};
    if (capabilities.maxImageExtent.width == 0 || capabilities.maxImageExtent.height == 0) {
        return;
    }

    Destroy();

    CreateSwapchain(capabilities);
    CreateSemaphores();

    resource_ticks.clear();
    resource_ticks.resize(image_count);
}

bool Swapchain::AcquireNextImage() {
#ifdef __OPENORBIS__
    // Round robin, once the next image is off screen. Nothing is signalled: the copy to the
    // image doesn't wait on a present semaphore on the PS4 (PresentManager::CopyToSwapchain).
    // Time spent waiting for the GPU (and VideoOut) before reusing an image: a large share of
    // each second means the PS4's GPU is the limit (reported by yuzu_cmd's perf line).
    if (g_ps4_loading_thread.joinable())
        StopLoadingScreenPS4(); // the game draws from now on
    const auto wait_start = std::chrono::steady_clock::now();
    WaitForImageOffScreenPS4();
    image_index = (image_index + 1) % image_count;
    scheduler.Wait(resource_ticks[image_index]);
    g_ps4_gpu_wait_us.fetch_add(u64(std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - wait_start).count()),
                                std::memory_order_relaxed);
    resource_ticks[image_index] = scheduler.CurrentTick();
    return false;
#endif
    const VkResult result = device.GetLogical().AcquireNextImageKHR(
        *swapchain, (std::numeric_limits<u64>::max)(), *present_semaphores[frame_index],
        VK_NULL_HANDLE, &image_index);
    switch (result) {
    case VK_SUCCESS:
        break;
    case VK_SUBOPTIMAL_KHR:
        is_suboptimal = true;
        break;
    case VK_ERROR_OUT_OF_DATE_KHR:
        is_outdated = true;
        break;
    case VK_ERROR_SURFACE_LOST_KHR:
        vk::Check(result);
        break;
    default:
        LOG_ERROR(Render_Vulkan, "vkAcquireNextImageKHR returned {}", string_VkResult(result));
        break;
    }

    const auto wait_with_frame_pacing = [this] {
    switch (Settings::values.frame_pacing_mode.GetValue()) {
    case Settings::FramePacingMode::Target_Auto:
        scheduler.Wait(resource_ticks[image_index]);
        break;
    case Settings::FramePacingMode::Target_30:
        scheduler.Wait(resource_ticks[image_index], 30.0);
        break;
    case Settings::FramePacingMode::Target_60:
        scheduler.Wait(resource_ticks[image_index], 60.0);
        break;
    case Settings::FramePacingMode::Target_90:
        scheduler.Wait(resource_ticks[image_index], 90.0);
        break;
    case Settings::FramePacingMode::Target_120:
        scheduler.Wait(resource_ticks[image_index], 120.0);
        break;
    }
    };

#ifdef __ANDROID__
    if (android_get_device_api_level() >= 30) {
        scheduler.Wait(resource_ticks[image_index]);
    } else {
        wait_with_frame_pacing();
    }
#else
    wait_with_frame_pacing();
#endif

    resource_ticks[image_index] = scheduler.CurrentTick();

    return is_suboptimal || is_outdated;
}

void Swapchain::Present(VkSemaphore render_semaphore) {
#ifdef __OPENORBIS__
    {
        std::scoped_lock lock{scheduler.submit_mutex};
        // No render semaphore on the PS4: the flip below is queued behind everything already
        // submitted to the graphics queue, including the copy into this image.
        (void)render_semaphore;
        const s64 arg = s64(ps4_flips + 1);
        int r = ac_ps4_submit_flip(g_ps4_video, image_index, PS4FlipVsync, arg);
        const bool gpu_flip = r == 0;
        if (!gpu_flip) {
            device.GetLogical().WaitIdle();
            r = sceVideoOutSubmitFlip(g_ps4_video, s32(image_index), PS4FlipVsync, arg);
        }
        // Lag spikes: a gap of 100 ms or more between presents, with what happened since the last
        // reported one (fastmem faults, new pipelines).
        {
            static auto s_last = std::chrono::steady_clock::now();
            static unsigned long long s_faults = 0, s_pipelines = 0;
            static u32 s_reported = 0;
            const auto now = std::chrono::steady_clock::now();
            const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_last).count();
            s_last = now;
            if (gap >= 100 && ps4_flips > 3 && s_reported < 200) {
                s_reported++;
                const unsigned long long faults = ps4_fastmem_faults.load(), pipelines = ps4_pipelines_built.load();
                PS4Trace("frame: %lld ms gap before present %llu (since the last gap: %llu fastmem faults, %llu new "
                         "pipelines)",
                         (long long)gap, (unsigned long long)ps4_flips + 1, faults - s_faults, pipelines - s_pipelines);
                s_faults = faults;
                s_pipelines = pipelines;
            }
        }
        if (r < 0) {
            LOG_ERROR(Render_Vulkan, "PS4 flip failed: {:#x}", u32(r));
        } else {
            ps4_flips++;
            g_ps4_presented = true;
            if (ps4_flips <= 3)
                PS4Trace("vulkan: present %llu (image %u, %s flip)", (unsigned long long)ps4_flips, image_index,
                         gpu_flip ? "GPU-side" : "CPU");
        }
    }
    if (++frame_index >= image_count)
        frame_index = 0;
    return;
#endif
    const auto present_queue{device.GetPresentQueue()};
    const VkPresentInfoKHR present_info{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = nullptr,
        .waitSemaphoreCount = render_semaphore ? 1U : 0U,
        .pWaitSemaphores = &render_semaphore,
        .swapchainCount = 1,
        .pSwapchains = swapchain.address(),
        .pImageIndices = &image_index,
        .pResults = nullptr,
    };
    std::scoped_lock lock{scheduler.submit_mutex};
    switch (const VkResult result = present_queue.Present(present_info)) {
    case VK_SUCCESS:
        break;
    case VK_SUBOPTIMAL_KHR:
        LOG_DEBUG(Render_Vulkan, "Suboptimal swapchain");
        break;
    case VK_ERROR_OUT_OF_DATE_KHR:
        is_outdated = true;
        break;
    case VK_ERROR_SURFACE_LOST_KHR:
        vk::Check(result);
        break;
    default:
        LOG_CRITICAL(Render_Vulkan, "Failed to present with error {}", string_VkResult(result));
        break;
    }
    ++frame_index;
    if (frame_index >= image_count) {
        frame_index = 0;
    }
}

void Swapchain::CreateSwapchain(const VkSurfaceCapabilitiesKHR& capabilities) {
    const auto physical_device{device.GetPhysical()};
    const auto formats{physical_device.GetSurfaceFormatsKHR(VkSurfaceKHR(surface))};
    const auto present_modes = physical_device.GetSurfacePresentModesKHR(VkSurfaceKHR(surface));

    has_mailbox = std::find(present_modes.begin(), present_modes.end(), VK_PRESENT_MODE_MAILBOX_KHR)
                  != present_modes.end();
    has_imm = std::find(present_modes.begin(), present_modes.end(),
                        VK_PRESENT_MODE_IMMEDIATE_KHR) != present_modes.end();
    has_fifo_relaxed = std::find(present_modes.begin(), present_modes.end(),
                                 VK_PRESENT_MODE_FIFO_RELAXED_KHR) != present_modes.end();

    const VkCompositeAlphaFlagBitsKHR alpha_flags{ChooseAlphaFlags(capabilities)};
    surface_format = ChooseSwapSurfaceFormat(formats);
    present_mode = ChooseSwapPresentMode(has_imm, has_mailbox, has_fifo_relaxed);

    u32 requested_image_count{capabilities.minImageCount + 1};
    // Ensure Triple buffering if possible.
    if (capabilities.maxImageCount > 0) {
        if (requested_image_count > capabilities.maxImageCount) {
            requested_image_count = capabilities.maxImageCount;
        } else {
            requested_image_count =
                (std::max)(requested_image_count, (std::min)(3U, capabilities.maxImageCount));
        }
    } else {
        requested_image_count = (std::max)(requested_image_count, 3U);
    }
    VkSwapchainCreateInfoKHR swapchain_ci{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = 0,
        .surface = VkSurfaceKHR(surface),
        .minImageCount = requested_image_count,
        .imageFormat = surface_format.format,
        .imageColorSpace = surface_format.colorSpace,
        .imageExtent = {},
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
#ifdef __ANDROID__
        // On Android, do not allow surface rotation to deviate from the frontend.
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
#else
        .preTransform = capabilities.currentTransform,
#endif
        .compositeAlpha = alpha_flags,
        .presentMode = present_mode,
        .clipped = VK_FALSE,
        .oldSwapchain = VkSwapchainKHR{},
    };
    const u32 graphics_family{device.GetGraphicsFamily()};
    const u32 present_family{device.GetPresentFamily()};
    const std::array<u32, 2> queue_indices{graphics_family, present_family};
    if (graphics_family != present_family) {
        swapchain_ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapchain_ci.queueFamilyIndexCount = static_cast<u32>(queue_indices.size());
        swapchain_ci.pQueueFamilyIndices = queue_indices.data();
    }
    // According to Vulkan spec, when using VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR,
    // the base format (imageFormat) MUST be included in pViewFormats
    const std::array view_formats{
        swapchain_ci.imageFormat,  // Base format MUST be first
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_B8G8R8A8_SRGB,
#ifdef __ANDROID__
        VK_FORMAT_R8G8B8A8_UNORM,  // Android may use RGBA
        VK_FORMAT_R8G8B8A8_SRGB,
#endif
    };
    VkImageFormatListCreateInfo format_list{
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR,
        .pNext = nullptr,
        .viewFormatCount = static_cast<u32>(view_formats.size()),
        .pViewFormats = view_formats.data(),
    };
    if (device.IsKhrSwapchainMutableFormatEnabled()) {
        format_list.pNext = std::exchange(swapchain_ci.pNext, &format_list);
        swapchain_ci.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
    }
    // Request the size again to reduce the possibility of a TOCTOU race condition.
    const auto updated_capabilities = physical_device.GetSurfaceCapabilitiesKHR(VkSurfaceKHR(surface));
    swapchain_ci.imageExtent = ChooseSwapExtent(updated_capabilities, width, height);
    // Don't add code within this and the swapchain creation.
    swapchain = device.GetLogical().CreateSwapchainKHR(swapchain_ci);

    extent = swapchain_ci.imageExtent;

    images = swapchain.GetImages();
    image_count = static_cast<u32>(images.size());
#ifdef __ANDROID__
    // Android is already ordered the same as Switch.
    image_view_format = VK_FORMAT_R8G8B8A8_UNORM;
#else
    image_view_format = VK_FORMAT_B8G8R8A8_UNORM;
#endif
}

void Swapchain::CreateSemaphores() {
    present_semaphores.resize(image_count);
    std::ranges::generate(present_semaphores,
                          [this] { return device.GetLogical().CreateSemaphore(); });
    render_semaphores.resize(image_count);
    std::ranges::generate(render_semaphores,
                          [this] { return device.GetLogical().CreateSemaphore(); });
}

void Swapchain::Destroy() {
#ifdef __OPENORBIS__
    DestroyPS4();
#endif
    frame_index = 0;
    present_semaphores.clear();
    render_semaphores.clear();
    swapchain.reset();
}

bool Swapchain::NeedsPresentModeUpdate() const {
    const auto requested_mode = ChooseSwapPresentMode(has_imm, has_mailbox, has_fifo_relaxed);
    return present_mode != requested_mode;
}

#ifdef __OPENORBIS__
void Swapchain::CreatePS4() {
    const auto& dld = device.GetDispatchLoader();
    const VkDevice dev = *device.GetLogical();
    if (g_ps4_video < 0) {
        g_ps4_video = sceVideoOutOpen(0xFF, 0, 0, nullptr);
        if (g_ps4_video < 0) {
            PS4Trace("vulkan: sceVideoOutOpen failed (%#x)", unsigned(g_ps4_video));
            is_outdated = true;
            return;
        }
        sceVideoOutSetFlipRate(g_ps4_video, 0);
    }
    surface_format = {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    image_view_format = VK_FORMAT_R8G8B8A8_UNORM;
    extent = {PS4Width, PS4Height};
    present_mode = VK_PRESENT_MODE_FIFO_KHR;

    const VkPhysicalDeviceMemoryProperties props = device.GetPhysical().GetMemoryProperties().memoryProperties;
    const auto get_layout = reinterpret_cast<PFN_vkGetImageSubresourceLayout>(
        dld.vkGetDeviceProcAddr(dev, "vkGetImageSubresourceLayout"));

    std::array<void*, PS4Images> addresses{};
    u32 pitch = PS4Width;
    images.clear();
    for (u32 i = 0; i < PS4Images; i++) {
        const VkImageCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = surface_format.format,
            .extent = {PS4Width, PS4Height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_LINEAR,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        VkImage image{};
        VkResult res = dld.vkCreateImage(dev, &info, nullptr, &image);
        VkMemoryRequirements req{};
        if (res == VK_SUCCESS)
            dld.vkGetImageMemoryRequirements(dev, image, &req);
        // VideoOut wants 2 MiB aligned buffers: over-allocate, bind at an aligned offset.
        u32 type = UINT32_MAX;
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (u32 t = 0; t < props.memoryTypeCount && type == UINT32_MAX; t++)
            if ((req.memoryTypeBits & (1u << t)) && (props.memoryTypes[t].propertyFlags & want) == want)
                type = t;
        VkDeviceMemory memory{};
        u8* base = nullptr;
        if (res == VK_SUCCESS && type != UINT32_MAX) {
            const VkMemoryAllocateInfo alloc{
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = req.size + PS4FramebufferAlign,
                .memoryTypeIndex = type,
            };
            res = dld.vkAllocateMemory(dev, &alloc, nullptr, &memory);
            if (res == VK_SUCCESS)
                res = dld.vkMapMemory(dev, memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&base));
        }
        if (res != VK_SUCCESS || !base) {
            PS4Trace("vulkan: swap image %u failed (VkResult %d, memory type %u)", i, int(res), type);
            if (image)
                dld.vkDestroyImage(dev, image, nullptr);
            if (memory)
                dld.vkFreeMemory(dev, memory, nullptr);
            is_outdated = true;
            return;
        }
        const uintptr_t base_address = reinterpret_cast<uintptr_t>(base);
        VkDeviceSize offset = Common::AlignUp(base_address, PS4FramebufferAlign) - base_address;
        offset = Common::AlignUp(offset, req.alignment);
        dld.vkBindImageMemory(dev, image, memory, offset);
        VkSubresourceLayout layout{};
        if (get_layout) {
            const VkImageSubresource subresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
            get_layout(dev, image, &subresource, &layout);
        } else {
            layout.rowPitch = PS4Width * 4;
        }
        addresses[i] = base + offset + layout.offset;
        pitch = u32(layout.rowPitch / 4);
        images.push_back(image);
        ps4_memory.push_back(memory);
    }
    image_count = PS4Images;

    struct { // OrbisVideoOutBufferAttribute
        s32 format, tmode, aspect;
        u32 width, height, pixel_pitch;
        u64 reserved[2];
    } attribute{s32(0x80002200) /* A8B8G8R8_SRGB */, 1 /* linear */, 0, PS4Width, PS4Height, pitch, {}};
    ps4_buffer_set = sceVideoOutRegisterBuffers(g_ps4_video, 0, addresses.data(), s32(PS4Images), &attribute);
    PS4Trace("vulkan: PS4 swap chain %ux%u, %u images, pitch %u, VideoOut buffers %s (%#x)", PS4Width, PS4Height,
             PS4Images, pitch, ps4_buffer_set >= 0 ? "registered" : "FAILED", unsigned(ps4_buffer_set));
    if (ps4_buffer_set < 0) {
        is_outdated = true;
    } else {
        g_ps4_swapchain = this;
        for (u32 i = 0; i < PS4Images; i++)
            g_ps4_addresses[i] = static_cast<uint32_t*>(addresses[i]);
        g_ps4_loading_pitch = pitch;
    }
    if (ps4_buffer_set >= 0 && !g_ps4_presented) {
        g_ps4_loading_dst = static_cast<uint32_t*>(addresses[0]);
        g_ps4_loading_pitch = pitch;
        StartLoadingScreenPS4();
    }
}

void Swapchain::DestroyPS4() {
    const auto& dld = device.GetDispatchLoader();
    const VkDevice dev = *device.GetLogical();
    if (!images.empty() || !ps4_memory.empty())
        device.GetLogical().WaitIdle();
    StopLoadingScreenPS4();
    g_ps4_loading_dst = nullptr;
    if (g_ps4_swapchain == this)
        g_ps4_swapchain = nullptr;
    if (ps4_buffer_set >= 0) {
        sceVideoOutUnregisterBuffers(g_ps4_video, ps4_buffer_set);
        ps4_buffer_set = -1;
    }
    for (VkImage image : images)
        dld.vkDestroyImage(dev, image, nullptr);
    for (VkDeviceMemory memory : ps4_memory)
        dld.vkFreeMemory(dev, memory, nullptr);
    images.clear();
    ps4_memory.clear();
}

// The next image must be off screen before it's drawn into: with 3 images in rotation, the one
// after the current is shown by flip n-2 and replaced on screen by flip n-1 (n = flips so far).
bool Swapchain::OverlayBeginPS4() {
    if (ps4_buffer_set < 0 || !g_ps4_presented || images.size() != PS4Images)
        return false;
    {
        std::scoped_lock lock{scheduler.submit_mutex};
        device.GetLogical().WaitIdle();
    }
    PS4FlipStatus status{};
    int shown = int(image_index); // the last presented image
    if (sceVideoOutGetFlipStatus(g_ps4_video, &status) >= 0 && status.current_buffer >= 0 &&
        status.current_buffer < int(PS4Images))
        shown = status.current_buffer;
    const int next = int((image_index + 1) % PS4Images); // the game's next image
    int spare = 0;
    while (spare == shown || spare == next)
        spare++;
    overlay_shown = shown;
    overlay_spare = spare;
    return true;
}

void Swapchain::OverlayDrawPS4(void (*draw)(uint32_t* dst, const uint32_t* src, int pitch, void* ctx), void* ctx) {
    if (overlay_spare < 0 || overlay_shown < 0)
        return;
    draw(g_ps4_addresses[size_t(overlay_spare)], g_ps4_addresses[size_t(overlay_shown)], int(g_ps4_loading_pitch), ctx);
    // The game's own flip count as the argument: WaitForImageOffScreenPS4 keeps working.
    sceVideoOutSubmitFlip(g_ps4_video, overlay_spare, PS4FlipVsync, s64(ps4_flips));
}

void Swapchain::OverlayEndPS4() {
    if (overlay_shown >= 0)
        sceVideoOutSubmitFlip(g_ps4_video, overlay_shown, PS4FlipVsync, s64(ps4_flips));
    overlay_shown = overlay_spare = -1;
}

void Swapchain::WaitForImageOffScreenPS4() {
    if (ps4_flips < 2)
        return;
    const s64 needed = s64(ps4_flips - 1);
    for (int tries = 0; tries < 100; tries++) {
        PS4FlipStatus status{};
        if (sceVideoOutGetFlipStatus(g_ps4_video, &status) < 0 || status.flip_arg >= needed)
            return;
        usleep(1000);
    }
}
#endif

} // namespace Vulkan

#ifdef __OPENORBIS__
// Redraws the loading screen (image 0) with a progress line, until the game's first frame.
extern "C" void ps4_swapchain_show_loading(const char* progress) {
    using namespace Vulkan;
    std::scoped_lock lk{g_ps4_loading_mutex};
    if (g_ps4_loading_thread.joinable() || !g_ps4_loading_dst || g_ps4_presented || !ps4_status_render_loading ||
        g_ps4_video < 0)
        return; // the loading screen thread redraws it
    ps4_status_render_loading(g_ps4_loading_dst, int(g_ps4_loading_pitch), progress);
    sceVideoOutSubmitFlip(g_ps4_video, 0, PS4FlipVsync, 0);
}
#endif

#ifdef __OPENORBIS__
extern "C" bool ps4_swapchain_overlay_begin() {
    return Vulkan::g_ps4_swapchain && Vulkan::g_ps4_swapchain->OverlayBeginPS4();
}
extern "C" void ps4_swapchain_overlay_draw(void (*draw)(uint32_t*, const uint32_t*, int, void*), void* ctx) {
    if (Vulkan::g_ps4_swapchain)
        Vulkan::g_ps4_swapchain->OverlayDrawPS4(draw, ctx);
}
extern "C" void ps4_swapchain_overlay_end() {
    if (Vulkan::g_ps4_swapchain)
        Vulkan::g_ps4_swapchain->OverlayEndPS4();
}
#endif

#ifdef __OPENORBIS__
extern "C" unsigned long long ps4_gpu_wait_us() {
    return Vulkan::g_ps4_gpu_wait_us.load(std::memory_order_relaxed);
}

// Whether a game frame has reached the screen yet (yuzu_cmd: a boot that stalls before it).
extern "C" bool ps4_first_frame_shown() {
    return Vulkan::g_ps4_presented.load(std::memory_order_relaxed);
}
#endif
