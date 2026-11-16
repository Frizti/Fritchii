// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <optional>
#include <tuple>
#include <unordered_set>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <dxgi1_4.h>
#endif

#include <cstring>
#include <iterator>
#include <string>

#include <fmt/format.h>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/nx2_hitch.h"
#include "common/common_types.h"
#include "common/literals.h"
#include "common/logging.h"
#include "common/memory_detect.h"
#include <ranges>
#include "video_core/memory_stage_link.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/vulkan_common/unbacked_fallback.h"
#include "video_core/vulkan_common/vma.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/gpu_logging/gpu_logging.h"
#include "common/settings.h"

namespace Vulkan {
namespace {

// Helpers translating MemoryUsage to flags/usage

    [[maybe_unused]] VkMemoryPropertyFlags MemoryUsagePropertyFlags(MemoryUsage usage) {
        switch (usage) {
            case MemoryUsage::DeviceLocal:
                return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            case MemoryUsage::Upload:
                return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            case MemoryUsage::Download:
                return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            case MemoryUsage::Stream:
                return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        }
        ASSERT_MSG(false, "Invalid memory usage={}", usage);
        return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }

    [[nodiscard]] VkMemoryPropertyFlags MemoryUsagePreferredVmaFlags(MemoryUsage usage) {
        if (usage == MemoryUsage::Download) {
            return VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        }
        return usage != MemoryUsage::DeviceLocal ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                    : VkMemoryPropertyFlagBits{};
    }

    [[nodiscard]] VmaAllocationCreateFlags MemoryUsageVmaFlags(MemoryUsage usage) {
        switch (usage) {
            case MemoryUsage::Upload:
            case MemoryUsage::Stream:
                return VMA_ALLOCATION_CREATE_MAPPED_BIT |
                        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            case MemoryUsage::Download:
                return VMA_ALLOCATION_CREATE_MAPPED_BIT |
                        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
            case MemoryUsage::DeviceLocal:
                return {};
        }
        return {};
    }

    [[nodiscard]] VmaMemoryUsage MemoryUsageVma(MemoryUsage usage) {
        switch (usage) {
            case MemoryUsage::DeviceLocal:
            case MemoryUsage::Stream:
                return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            case MemoryUsage::Upload:
            case MemoryUsage::Download:
                return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        }
        return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }
} // namespace

//MemoryCommit is now VMA-backed
MemoryCommit::MemoryCommit(VmaAllocator alloc, VmaAllocation a,
                            const VmaAllocationInfo &info) noexcept
        : allocator{alloc}, allocation{a}, memory{info.deviceMemory},
            offset{info.offset}, size{info.size}, mapped_ptr{info.pMappedData} {
    // Log GPU memory allocation
    if (GPU::Logging::IsActive() &&
        Settings::values.gpu_log_memory_tracking.GetValue()) {
        GPU::Logging::GPULogger::GetInstance().LogMemoryAllocation(
            reinterpret_cast<uintptr_t>(memory),
            static_cast<u64>(size),
            0  // Memory property flags (not easily available from VMA)
        );
    }
}

MemoryCommit::~MemoryCommit() { Release(); }

MemoryCommit::MemoryCommit(MemoryCommit &&rhs) noexcept
        : allocator{std::exchange(rhs.allocator, nullptr)},
            allocation{std::exchange(rhs.allocation, nullptr)},
            memory{std::exchange(rhs.memory, VK_NULL_HANDLE)},
            offset{std::exchange(rhs.offset, 0)},
            size{std::exchange(rhs.size, 0)},
            mapped_ptr{std::exchange(rhs.mapped_ptr, nullptr)} {}

MemoryCommit &MemoryCommit::operator=(MemoryCommit &&rhs) noexcept {
    if (this != &rhs) {
        Release();
        allocator = std::exchange(rhs.allocator, nullptr);
        allocation = std::exchange(rhs.allocation, nullptr);
        memory = std::exchange(rhs.memory, VK_NULL_HANDLE);
        offset = std::exchange(rhs.offset, 0);
        size = std::exchange(rhs.size, 0);
        mapped_ptr = std::exchange(rhs.mapped_ptr, nullptr);
    }
    return *this;
}

std::span<u8> MemoryCommit::Map()
{
    if (!allocation) return {};
    if (!mapped_ptr) {
        if (vmaMapMemory(allocator, allocation, &mapped_ptr) != VK_SUCCESS) return {};
    }
    const size_t n = static_cast<size_t>(std::min<VkDeviceSize>(size,
                                                                (std::numeric_limits<size_t>::max)()));
    return std::span<u8>{static_cast<u8 *>(mapped_ptr), n};
}

std::span<const u8> MemoryCommit::Map() const
{
    if (!allocation) return {};
    if (!mapped_ptr) {
        void *p = nullptr;
        if (vmaMapMemory(allocator, allocation, &p) != VK_SUCCESS) return {};
        const_cast<MemoryCommit *>(this)->mapped_ptr = p;
    }
    const size_t n = static_cast<size_t>(std::min<VkDeviceSize>(size,
                                                                (std::numeric_limits<size_t>::max)()));
    return std::span<const u8>{static_cast<const u8 *>(mapped_ptr), n};
}

void MemoryCommit::Unmap()
{
    if (allocation && mapped_ptr) {
        vmaUnmapMemory(allocator, allocation);
        mapped_ptr = nullptr;
    }
}

void MemoryCommit::Release() {
    if (allocation && allocator) {
        const VideoCommon::MemoryStageLink::AllocatorOp allocator_op{allocator};
        // Log GPU memory deallocation
        if (GPU::Logging::IsActive() &&
            Settings::values.gpu_log_memory_tracking.GetValue() &&
            memory != VK_NULL_HANDLE) {
            GPU::Logging::GPULogger::GetInstance().LogMemoryDeallocation(
                reinterpret_cast<uintptr_t>(memory)
            );
        }

        if (mapped_ptr) {
            vmaUnmapMemory(allocator, allocation);
            mapped_ptr = nullptr;
        }
        vmaFreeMemory(allocator, allocation);
    }
    allocation = nullptr;
    allocator = nullptr;
    memory = VK_NULL_HANDLE;
    offset = 0;
    size = 0;
}

MemoryAllocator::MemoryAllocator(const Device &device_, bool buffers)
        : device{device_}, allocator{buffers ? device.GetBufferAllocator() : device.GetAllocator()},
            properties{device_.GetPhysical().GetMemoryProperties().memoryProperties},
            buffer_image_granularity{
                    device_.GetPhysical().GetProperties().limits.bufferImageGranularity} {

    // Preserve the previous "RenderDoc small heap" trimming behavior that we had in original vma minus the heap bug
    if (device.HasDebuggingToolAttached())
    {
        using namespace Common::Literals;
        ForEachDeviceLocalHostVisibleHeap(device, [this](size_t heap_idx, VkMemoryHeap &heap) {
            if (heap.size <= 256_MiB) {
                for (u32 t = 0; t < properties.memoryTypeCount; ++t) {
                    if (properties.memoryTypes[t].heapIndex == heap_idx) {
                        valid_memory_types &= ~(1u << t);
                    }
                }
            }
        });
    }
}

MemoryAllocator::~MemoryAllocator() = default;

vk::Image MemoryAllocator::CreateImage(const VkImageCreateInfo &ci, bool dedicated) const
{
    vk::Image image;
    const u64 start = Common::Nx2Hitch::NowNs();
    vk::Check(AllocateImage(ci, dedicated, false, image));
    Common::Nx2Hitch::Add(Common::Nx2Hitch::Cause::ImageCreate, Common::Nx2Hitch::NowNs() - start);
    return image;
}

vk::Image MemoryAllocator::TryCreateImage(const VkImageCreateInfo &ci, bool dedicated) const {
    vk::Image image;
    const u64 start = Common::Nx2Hitch::NowNs();
    const VkResult result = AllocateImage(ci, dedicated, true, image);
    Common::Nx2Hitch::Add(Common::Nx2Hitch::Cause::ImageCreate, Common::Nx2Hitch::NowNs() - start);
    if (result != VK_ERROR_OUT_OF_DEVICE_MEMORY && result != VK_ERROR_OUT_OF_HOST_MEMORY) {
        vk::Check(result);
    }
    return image;
}

namespace {
std::atomic<u64> g_timed_images{};
std::atomic<u64> g_timed_create_ns{};
std::atomic<u64> g_timed_alloc_ns{};
std::atomic<u64> g_timed_bind_ns{};
std::atomic<u64> g_timed_new_block{};    ///< Images whose create added a VMA memory object
std::atomic<u64> g_timed_new_block_ns{}; ///< ... their create, allocate and bind time
std::atomic<u64> g_timed_dedicated{};    ///< ... of them, dedicated allocations

u64 TimingNow() noexcept {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

// vmaCreateImage in its three steps (as VMA does them, without its dedicated-allocation hint
// queries, which VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT callers set themselves), each timed.
VkResult TimedVmaCreateImage(const Device &device, VmaAllocator allocator,
                             const VkImageCreateInfo &ci, const VmaAllocationCreateInfo &alloc_ci,
                             VkImage &handle, VmaAllocation &allocation,
                             VmaAllocationInfo &alloc_info) {
    const auto &dld = device.GetDispatchLoader();
    const VkDevice logical = *device.GetLogical();
    // Outside the timed steps: did this image take a memory object VMA did not hold before?
    const u64 blocks_before = vk::VmaMemoryObjectCount(allocator);
    const u64 t0 = TimingNow();
    VkResult result = dld.vkCreateImage(logical, &ci, nullptr, &handle);
    const u64 t1 = TimingNow();
    if (result != VK_SUCCESS) {
        handle = VK_NULL_HANDLE;
        return result;
    }
    // VMA_MEMORY_USAGE_AUTO* needs the create info (vmaCreateImage only; here it fails with
    // VK_ERROR_FEATURE_NOT_PRESENT): the same preference without it.
    VmaAllocationCreateInfo explicit_ci = alloc_ci;
    if (explicit_ci.usage == VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE ||
        explicit_ci.usage == VMA_MEMORY_USAGE_AUTO) {
        explicit_ci.usage = VMA_MEMORY_USAGE_UNKNOWN;
        explicit_ci.preferredFlags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    } else if (explicit_ci.usage == VMA_MEMORY_USAGE_AUTO_PREFER_HOST) {
        explicit_ci.usage = VMA_MEMORY_USAGE_UNKNOWN;
    }
    result = vmaAllocateMemoryForImage(allocator, handle, &explicit_ci, &allocation, &alloc_info);
    const u64 t2 = TimingNow();
    if (result == VK_SUCCESS) {
        result = vmaBindImageMemory(allocator, allocation, handle);
        if (result != VK_SUCCESS) {
            vmaFreeMemory(allocator, allocation);
        }
    }
    const u64 t3 = TimingNow();
    if (result != VK_SUCCESS) {
        dld.vkDestroyImage(logical, handle, nullptr);
        handle = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
        return result;
    }
    g_timed_images.fetch_add(1, std::memory_order_relaxed);
    g_timed_create_ns.fetch_add(t1 - t0, std::memory_order_relaxed);
    g_timed_alloc_ns.fetch_add(t2 - t1, std::memory_order_relaxed);
    g_timed_bind_ns.fetch_add(t3 - t2, std::memory_order_relaxed);
    if (vk::VmaMemoryObjectCount(allocator) > blocks_before) {
        g_timed_new_block.fetch_add(1, std::memory_order_relaxed);
        g_timed_new_block_ns.fetch_add(t3 - t0, std::memory_order_relaxed);
        VmaAllocationInfo2 info2{};
        vmaGetAllocationInfo2(allocator, allocation, &info2);
        if (info2.dedicatedMemory) {
            g_timed_dedicated.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Per image, the slow ones (over 500 us in all): is the memory a block VMA had already (a hole
    // reused) or one it just took (fable_answer_npcdips_2 section 1)? 400 lines at most.
    static std::mutex blocks_mutex;
    static std::unordered_set<VkDeviceMemory> blocks_seen;
    static u32 slow_logged = 0;
    bool new_block = false;
    {
        std::scoped_lock lock{blocks_mutex};
        new_block = blocks_seen.insert(alloc_info.deviceMemory).second;
        if (t3 - t0 < 500'000 || slow_logged >= 400) {
            return VK_SUCCESS;
        }
        ++slow_logged;
    }
    LOG_INFO(Render_Vulkan,
             "Slow image create: {}x{} format {} usage {:#x}, {} KiB at offset {} KiB of {} "
             "block; vkCreateImage {} us, allocate {} us, bind {} us",
             ci.extent.width, ci.extent.height, static_cast<u32>(ci.format), ci.usage,
             alloc_info.size >> 10, alloc_info.offset >> 10, new_block ? "a NEW" : "an old",
             (t1 - t0) / 1000, (t2 - t1) / 1000, (t3 - t2) / 1000);
    return VK_SUCCESS;
}
} // Anonymous namespace

// EDEN_NX2_DEPTH_POOL (boot, on by default, =0 off): depth/stencil attachments come from their own VMA
// pool per memory type, so the memory a freed depth target leaves is reused by depth targets only.
// The NPC-pass dips (night market): a new D32 target nearly every frame, and its vmaBindImageMemory
// took 1.35 ms on average against 0.11 ms for colour targets (IMAGE_ALLOC_TIMING): the driver
// readies the memory for depth on each bind, which memory that held depth before may skip. The
// pools live as long as the VMA allocator (never destroyed: images may outlive this object).
static VmaPool DepthAttachmentPool(VmaAllocator allocator, const VkImageCreateInfo &ci,
                                   const VmaAllocationCreateInfo &alloc_ci) {
    // On by default since 2026-11-12: depth targets no longer each get their own fresh memory (the
    // driver prefers dedicated memory for them outside a pool). Night market, camera panning,
    // 120 s: binds over 5 ms 6 -> 0 (worst 162 ms -> 1.8 ms), hitches over 50 ms 5 -> 1.
    static const bool enabled = [] {
        const char *value = std::getenv("EDEN_NX2_DEPTH_POOL");
        return value == nullptr || value[0] == '\0' || value[0] != '0';
    }();
    if (!enabled) {
        return VK_NULL_HANDLE;
    }
    u32 type = 0;
    if (vmaFindMemoryTypeIndexForImageInfo(allocator, &ci, &alloc_ci, &type) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    static std::mutex mutex;
    static std::vector<std::tuple<VmaAllocator, u32, VmaPool>> pools;
    std::scoped_lock lock{mutex};
    for (const auto &[owner, pool_type, pool] : pools) {
        if (owner == allocator && pool_type == type) {
            return pool;
        }
    }
    using namespace Common::Literals;
    // EDEN_NX2_DEPTH_POOL_MIN_BLOCKS (boot, default 0; try 4 = 256 MiB): blocks the pool never gives
    // back. VMA frees an empty block, so with 0 the pool still asked the driver for new memory at
    // each peak (night market panning: 39 images on a new block, 4 binds over 20 ms, worst 67 ms).
    static const u32 min_blocks = [] {
        const char *value = std::getenv("EDEN_NX2_DEPTH_POOL_MIN_BLOCKS");
        return value != nullptr ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 0U;
    }();
    const VmaPoolCreateInfo pool_ci{
        .memoryTypeIndex = type,
        .flags = 0,
        .blockSize = 64_MiB,
        .minBlockCount = min_blocks,
        .maxBlockCount = 0,
        .priority = 0.f,
        .minAllocationAlignment = 0,
        .pMemoryAllocateNext = nullptr,
    };
    VmaPool pool{};
    if (vmaCreatePool(allocator, &pool_ci, &pool) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    LOG_INFO(Render_Vulkan, "Depth attachment pool (EDEN_NX2_DEPTH_POOL) for memory type {}", type);
    pools.emplace_back(allocator, type, pool);
    return pool;
}

namespace {

// FRITCHII_KILL_RT_POOL (live, default 0): colour render targets from their own VMA pool, as
// EDEN_NX2_DEPTH_POOL does for depth. Outside a pool the driver's preference gives a colour
// target its own memory, so each new one is a driver allocation on the GPU back thread. In the
// owner's Stalchild fights (fight-keepers*, 2026-11-14) the only frames over 50 ms after the load
// were single image creates of 35 and 124 ms right at kills (one of 159 ms in a playtest), with
// the back blocked and the worker, fencing and present threads idle behind it. 1 = colour targets
// of up to 32 MiB are made with vkCreateImage, allocated from the pool (64 MiB blocks) and bound;
// an image the pool refuses goes the old way. 2 = dry: nothing moves, the census below counts
// the candidates and how many of them got memory of their own. Only memory placement changes,
// never what is drawn. Images made before a switch change stay where they are.
int KillRtPoolMode() {
    return VideoCore::Nx2Debug::SwitchValue(VideoCore::Nx2Debug::g_kill_rt_pool,
                                            "FRITCHII_KILL_RT_POOL");
}

constexpr VkDeviceSize KillPoolBlockBytes = VkDeviceSize{64} << 20;
constexpr VkDeviceSize KillPoolMaxBytes = KillPoolBlockBytes / 2;

bool IsKillPoolCandidate(const VkImageCreateInfo& ci, bool dedicated) {
    constexpr VkImageCreateFlags refused = VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
                                           VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                                           VK_IMAGE_CREATE_SPARSE_ALIASED_BIT |
                                           VK_IMAGE_CREATE_DISJOINT_BIT;
    return !dedicated && (ci.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
           (ci.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0 &&
           (ci.flags & refused) == 0 && ci.imageType == VK_IMAGE_TYPE_2D;
}

struct KillPoolEntry {
    VmaAllocator owner;
    u32 type;
    VmaPool pool;
};
std::mutex g_kill_pool_mutex;
std::vector<KillPoolEntry> g_kill_pools;

// One pool per allocator and memory type, never destroyed (images may outlive the allocator
// object). FRITCHII_KILL_RT_POOL_BLOCKS (boot, default 2, at most 8 = 512 MiB): blocks the pool
// makes at once and never gives back, so the first targets of a fight find memory already made.
VmaPool ColourAttachmentPool(VmaAllocator allocator, const VkImageCreateInfo& ci,
                             const VmaAllocationCreateInfo& alloc_ci) {
    u32 type = 0;
    if (vmaFindMemoryTypeIndexForImageInfo(allocator, &ci, &alloc_ci, &type) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    std::scoped_lock lock{g_kill_pool_mutex};
    for (const KillPoolEntry& entry : g_kill_pools) {
        if (entry.owner == allocator && entry.type == type) {
            return entry.pool;
        }
    }
    static const u32 min_blocks = [] {
        const char* value = std::getenv("FRITCHII_KILL_RT_POOL_BLOCKS");
        const unsigned long blocks = value != nullptr ? std::strtoul(value, nullptr, 10) : 2UL;
        return static_cast<u32>((std::min)(blocks, 8UL));
    }();
    const VmaPoolCreateInfo pool_ci{
        .memoryTypeIndex = type,
        .flags = 0,
        .blockSize = KillPoolBlockBytes,
        .minBlockCount = min_blocks,
        .maxBlockCount = 0,
        .priority = 0.f,
        .minAllocationAlignment = 0,
        .pMemoryAllocateNext = nullptr,
    };
    VmaPool pool{};
    if (vmaCreatePool(allocator, &pool_ci, &pool) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    LOG_INFO(Render_Vulkan,
             "Colour target pool (FRITCHII_KILL_RT_POOL) for memory type {}: 64 MiB blocks, {} kept",
             type, min_blocks);
    g_kill_pools.push_back({allocator, type, pool});
    return pool;
}

// What the census sorts image creates into.
enum KillKind : u32 {
    KillColourPool,  ///< a colour target in the FRITCHII_KILL_RT_POOL pool
    KillColourOwn,   ///< a colour target with memory of its own (a driver allocation)
    KillColourBlock, ///< a colour target in a shared VMA block
    KillDepthOwn,
    KillDepthBlock, ///< includes EDEN_NX2_DEPTH_POOL
    KillOtherOwn,
    KillOtherBlock,
    KillKinds,
};
constexpr std::array<const char*, KillKinds> kill_kind_names{
    "colour in the kill pool", "colour own memory", "colour in a block", "depth own memory",
    "depth in a block",        "other own memory",  "other in a block"};

struct KillCensus {
    std::array<u64, KillKinds> count{};
    std::array<u64, KillKinds> slow5{};
    std::array<u64, KillKinds> slow20{};
    std::array<u64, KillKinds> ns{};
    std::array<u64, KillKinds> longest{};
    u64 worst_ns = 0;
    VkExtent3D worst_extent{};
    u32 worst_format = 0;
    u32 worst_usage = 0;
    u32 worst_kind = 0;
    u64 placed = 0;      ///< taken by the pool
    u64 too_big = 0;     ///< over KillPoolMaxBytes: went the old way
    u64 refused = 0;     ///< the pool or its bind failed: went the old way
    u64 candidates = 0;  ///< dry mode: colour targets the pool would have tried
    u64 dry_own = 0;     ///< ... of them given memory of their own
    u64 window_start = 0;
};
std::mutex g_census_mutex;
KillCensus g_census;

// vkCreateImage, then the pool's memory, then the bind. False with nothing left behind when the
// pool does not take the image (too large, no pool, no room): the caller goes the old way.
bool TryKillPool(const Device& device, VmaAllocator allocator, const VkImageCreateInfo& ci,
                 const VmaAllocationCreateInfo& alloc_ci, VkImage& handle,
                 VmaAllocation& allocation, VmaAllocationInfo& alloc_info, bool& too_big) {
    too_big = false;
    const VmaPool pool = ColourAttachmentPool(allocator, ci, alloc_ci);
    if (pool == VK_NULL_HANDLE) {
        return false;
    }
    const auto& dld = device.GetDispatchLoader();
    const VkDevice logical = *device.GetLogical();
    VkImage image{};
    if (dld.vkCreateImage(logical, &ci, nullptr, &image) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements requirements{};
    dld.vkGetImageMemoryRequirements(logical, image, &requirements);
    if (requirements.size > KillPoolMaxBytes) {
        dld.vkDestroyImage(logical, image, nullptr);
        too_big = true;
        return false;
    }
    // A pool fixes the memory type; usage and preferences are not read (the AUTO usages would
    // need vmaCreateImage's create info, see TimedVmaCreateImage).
    VmaAllocationCreateInfo pool_ci = alloc_ci;
    pool_ci.flags &= ~VmaAllocationCreateFlags{VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT};
    pool_ci.usage = VMA_MEMORY_USAGE_UNKNOWN;
    pool_ci.pool = pool;
    VmaAllocation pooled{};
    VmaAllocationInfo pooled_info{};
    if (vmaAllocateMemoryForImage(allocator, image, &pool_ci, &pooled, &pooled_info) !=
        VK_SUCCESS) {
        dld.vkDestroyImage(logical, image, nullptr);
        return false;
    }
    if (vmaBindImageMemory(allocator, pooled, image) != VK_SUCCESS) {
        vmaFreeMemory(allocator, pooled);
        dld.vkDestroyImage(logical, image, nullptr);
        return false;
    }
    handle = image;
    allocation = pooled;
    alloc_info = pooled_info;
    return true;
}

// Called after every image made with memory (same thread as the allocation: the allocator is
// externally synchronized, so the pool statistics below may be read here). Logs every 10 s
// while FRITCHII_KILL_RT_POOL is not 0, and at 0 only for a window that had a create of 20 ms
// or more.
void NoteKillCensus(VmaAllocator allocator, const VkImageCreateInfo& ci, VmaAllocation allocation,
                    u64 elapsed_ns, bool pooled, bool candidate, int mode) {
    bool own = false;
    if (allocation != VK_NULL_HANDLE) {
        VmaAllocationInfo2 info2{};
        vmaGetAllocationInfo2(allocator, allocation, &info2);
        own = info2.dedicatedMemory != VK_FALSE;
    }
    const bool colour = (ci.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    const bool depth = (ci.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    const u32 kind = pooled  ? KillColourPool
                     : depth ? (own ? KillDepthOwn : KillDepthBlock)
                     : colour ? (own ? KillColourOwn : KillColourBlock)
                              : (own ? KillOtherOwn : KillOtherBlock);
    const u64 now = Common::Nx2Hitch::NowNs();
    std::string line;
    {
        std::scoped_lock lock{g_census_mutex};
        KillCensus& c = g_census;
        if (c.window_start == 0) {
            c.window_start = now;
        }
        ++c.count[kind];
        c.ns[kind] += elapsed_ns;
        c.slow5[kind] += elapsed_ns >= 5'000'000 ? 1 : 0;
        c.slow20[kind] += elapsed_ns >= 20'000'000 ? 1 : 0;
        c.longest[kind] = (std::max)(c.longest[kind], elapsed_ns);
        if (elapsed_ns > c.worst_ns) {
            c.worst_ns = elapsed_ns;
            c.worst_extent = ci.extent;
            c.worst_format = static_cast<u32>(ci.format);
            c.worst_usage = ci.usage;
            c.worst_kind = kind;
        }
        c.placed += pooled ? 1 : 0;
        if (mode == 2 && candidate) {
            ++c.candidates;
            c.dry_own += own ? 1 : 0;
        }
        if (now - c.window_start < 10'000'000'000ULL) {
            return;
        }
        if (mode != 0 || c.worst_ns >= 20'000'000) {
            u64 made = 0;
            u64 total_ns = 0;
            std::string kinds;
            for (u32 k = 0; k < KillKinds; ++k) {
                made += c.count[k];
                total_ns += c.ns[k];
                if (c.count[k] == 0) {
                    continue;
                }
                kinds += fmt::format("{}{} {} ({} ms, {} over 5 ms, {} over 20 ms, longest {:.1f} ms)",
                                     kinds.empty() ? "" : "; ", kill_kind_names[k], c.count[k],
                                     c.ns[k] / 1'000'000, c.slow5[k], c.slow20[k],
                                     static_cast<double>(c.longest[k]) / 1e6);
            }
            u64 pool_blocks = 0;
            u64 pool_block_bytes = 0;
            u64 pool_used_bytes = 0;
            {
                std::scoped_lock pool_lock{g_kill_pool_mutex};
                for (const KillPoolEntry& entry : g_kill_pools) {
                    if (entry.owner != allocator) {
                        continue;
                    }
                    VmaStatistics stats{};
                    vmaGetPoolStatistics(allocator, entry.pool, &stats);
                    pool_blocks += stats.blockCount;
                    pool_block_bytes += stats.blockBytes;
                    pool_used_bytes += stats.allocationBytes;
                }
            }
            line = fmt::format(
                "Kill image creates (FRITCHII_KILL_RT_POOL={}) in 10 s: {} made in {} ms; {}; worst "
                "{:.1f} ms: {}x{}x{} format {} usage {:#x} ({}); kill pool: {} placed, {} over 32 "
                "MiB, {} refused, {} blocks, {} of {} MiB used; dry: {} colour targets the pool "
                "would try, {} of them got memory of their own",
                mode, made, total_ns / 1'000'000, kinds.empty() ? "none" : kinds,
                static_cast<double>(c.worst_ns) / 1e6, c.worst_extent.width,
                c.worst_extent.height, c.worst_extent.depth, c.worst_format, c.worst_usage,
                kill_kind_names[c.worst_kind], c.placed, c.too_big, c.refused, pool_blocks,
                pool_used_bytes >> 20, pool_block_bytes >> 20, c.candidates, c.dry_own);
        }
        c = KillCensus{};
        c.window_start = now;
    }
    if (!line.empty()) {
        LOG_INFO(Render_Vulkan, "{}", line);
    }
}

void NoteKillPoolMiss(bool too_big) {
    std::scoped_lock lock{g_census_mutex};
    if (too_big) {
        ++g_census.too_big;
    } else {
        ++g_census.refused;
    }
}

/// The first refusals of a session get the heaps in full: which heap was short, by how much, and
/// whether VMA follows the driver's budget ("over the memory budget" with 1.1 GB used could not
/// be explained from the log).
constexpr u32 RefusalDetailEvents = 4;

/// Every heap's driver usage and budget and VMA's blocks and allocations in it, in MiB.
std::string HeapBudgetsText(VmaAllocator allocator) {
    const VkPhysicalDeviceMemoryProperties* properties{};
    vmaGetMemoryProperties(allocator, &properties);
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
    vmaGetHeapBudgets(allocator, budgets.data());
    std::string text;
    for (u32 heap = 0; heap < properties->memoryHeapCount; ++heap) {
        const VmaBudget& budget = budgets[heap];
        fmt::format_to(std::back_inserter(text),
                       "{}heap {} ({}, {} MiB): usage {} of budget {}, VMA blocks {}, allocations {}",
                       heap == 0 ? "" : "; ", heap,
                       (properties->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0
                           ? "local"
                           : "host",
                       properties->memoryHeaps[heap].size >> 20, budget.usage >> 20,
                       budget.budget >> 20, budget.statistics.blockBytes >> 20,
                       budget.statistics.allocationBytes >> 20);
    }
    return text;
}

/// "type T (heap H, flags F)" of an allocation that was made.
std::string PlacementText(VmaAllocator allocator, u32 memory_type) {
    const VkPhysicalDeviceMemoryProperties* properties{};
    vmaGetMemoryProperties(allocator, &properties);
    if (memory_type >= properties->memoryTypeCount) {
        return fmt::format("type {}", memory_type);
    }
    const VkMemoryType& type = properties->memoryTypes[memory_type];
    return fmt::format("type {} (heap {}, flags {:#x})", memory_type, type.heapIndex,
                       static_cast<u32>(type.propertyFlags));
}

} // Anonymous namespace

bool MemoryAllocator::ImageTimingEnabled() noexcept {
    static const bool enabled = [] {
        const char *value = std::getenv("EDEN_NX2_IMAGE_ALLOC_TIMING");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

MemoryAllocator::ImageTiming MemoryAllocator::ReadImageTiming() noexcept {
    return ImageTiming{
        .images = g_timed_images.load(std::memory_order_relaxed),
        .create_ns = g_timed_create_ns.load(std::memory_order_relaxed),
        .alloc_ns = g_timed_alloc_ns.load(std::memory_order_relaxed),
        .bind_ns = g_timed_bind_ns.load(std::memory_order_relaxed),
        .new_block_images = g_timed_new_block.load(std::memory_order_relaxed),
        .new_block_ns = g_timed_new_block_ns.load(std::memory_order_relaxed),
        .dedicated_images = g_timed_dedicated.load(std::memory_order_relaxed),
    };
}

VkResult MemoryAllocator::AllocateImage(const VkImageCreateInfo &ci, bool dedicated, bool may_fail,
                                        vk::Image &image) const {
    const VideoCommon::MemoryStageLink::AllocatorOp allocator_op{allocator};
    const VmaAllocationCreateInfo alloc_ci = {
            // Best fit: images freed by the thousand leave holes in the blocks, which new ones
            // should fill before VMA takes another block.
            .flags = static_cast<VmaAllocationCreateFlags>(VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT |
                                                           VMA_ALLOCATION_CREATE_STRATEGY_MIN_MEMORY_BIT) |
                     (dedicated ? VmaAllocationCreateFlags{VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT}
                               : VmaAllocationCreateFlags{0}),
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .requiredFlags = 0,
            .preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            .memoryTypeBits = 0,
            .pool = VK_NULL_HANDLE,
            .pUserData = nullptr,
            .priority = 0.f,
    };

    VkImage handle{};
    VmaAllocation allocation{};
    VmaAllocationInfo alloc_info{};
    const u64 texels = u64{ci.extent.width} * ci.extent.height * ci.extent.depth * ci.arrayLayers;
    u64 bytes = 0;
    if (texels > MaxBackedImageTexels) {
        // Its memory size decides: an image the driver won't create is too large to back.
        const auto& dld = device.GetDispatchLoader();
        VkImage probe{};
        bytes = ~u64{0};
        if (dld.vkCreateImage(*device.GetLogical(), &ci, nullptr, &probe) == VK_SUCCESS) {
            bytes = device.GetLogical().GetImageMemoryRequirements(probe).size;
            dld.vkDestroyImage(*device.GetLogical(), probe, nullptr);
        }
    }
    const bool huge = IsTooLargeToBack(texels, bytes);
    // EDEN_NX2_DEPTH_POOL: the first attempt only; the fallbacks below take any memory.
    VmaAllocationCreateInfo first_ci = alloc_ci;
    if (!dedicated && (ci.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
        first_ci.pool = DepthAttachmentPool(allocator, ci, alloc_ci);
    }
    // FRITCHII_KILL_RT_POOL: 1 = colour targets try their pool first; 2 = dry (census only).
    const u64 kill_start = Common::Nx2Hitch::NowNs();
    const int kill_mode = KillRtPoolMode();
    const bool kill_candidate = !huge && IsKillPoolCandidate(ci, dedicated);
    bool kill_pooled = false;
    if (kill_candidate && kill_mode == 1) {
        bool too_big = false;
        kill_pooled = TryKillPool(device, allocator, ci, alloc_ci, handle, allocation, alloc_info,
                                  too_big);
        if (!kill_pooled) {
            NoteKillPoolMiss(too_big);
        }
    }
    VkResult result = kill_pooled ? VK_SUCCESS
                      : huge ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                      : ImageTimingEnabled()
                          ? TimedVmaCreateImage(device, allocator, ci, first_ci, handle,
                                                allocation, alloc_info)
                          : vmaCreateImage(allocator, &ci, &first_ci, &handle, &allocation, &alloc_info);
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY && !huge && first_ci.pool != VK_NULL_HANDLE &&
        !kill_pooled &&
        (VideoCore::Nx2Debug::SwitchValue(VideoCore::Nx2Debug::g_depth_pool_big,
                                          "FRITCHII_DEPTH_POOL_BIG") > 0 ||
         VideoCore::Nx2Debug::SwitchValue(VideoCore::Nx2Debug::g_depth_pool_retry,
                                          "FRITCHII_DEPTH_POOL_RETRY", 1) != 0)) {
        // Same fix from two helpers (BA: FRITCHII_DEPTH_POOL_BIG, BF: FRITCHII_DEPTH_POOL_RETRY);
        // either switch turns it on. On by default since 2026-11-16 (FRITCHII_DEPTH_POOL_RETRY=0
        // turns it off): only where the image is placed changes. Without it every OoT area load
        // logged a false "over the memory budget" (RX 9070 XT, 16 GB, 1.1 GB used) and the false
        // pressure event emptied the image pool and cut the cache's thresholds by 512 MiB.
        // FRITCHII_DEPTH_POOL_BIG: VMA refuses anything larger than a custom pool's block
        // (64 MiB) with VK_ERROR_OUT_OF_DEVICE_MEMORY before it looks at the budget, so a large
        // depth target (OoT, every Castle Town load: 12288x12288 D16, 288 MiB) read as "over
        // the memory budget": a false pressure event (+512 MiB kept free) and a placement
        // without the budget check. Such an image tries the normal heap first, budget kept.
        result = ImageTimingEnabled()
                     ? TimedVmaCreateImage(device, allocator, ci, alloc_ci, handle, allocation,
                                           alloc_info)
                     : vmaCreateImage(allocator, &ci, &alloc_ci, &handle, &allocation, &alloc_info);
        static std::atomic<u32> placed{};
        if (const u32 n = placed.fetch_add(1, std::memory_order_relaxed); n < 8) {
            LOG_INFO(Render_Vulkan,
                     "Depth image {}x{}x{} (format {}) refused by the depth pool, outside it: {} "
                     "(FRITCHII_DEPTH_POOL_BIG, {} so far)",
                     ci.extent.width, ci.extent.height, ci.arrayLayers,
                     static_cast<u32>(ci.format),
                     result == VK_SUCCESS ? "placed within the budget" : "over the budget too",
                     n + 1);
        }
    }
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY && !huge && device.KeptBlockBytes() != 0) {
        // FRITCHII_KEEP_BLOCKS: the kept blocks count against the budget VMA checks; they go
        // first, and VMA reads the driver's usage again before the retry.
        if (device.ReleaseKeptBlocks() != 0) {
            vmaSetCurrentFrameIndex(allocator, 0);
            result = vmaCreateImage(allocator, &ci, &first_ci, &handle, &allocation, &alloc_info);
        }
    }
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY && !huge) {
        // Over the device memory budget (Switch 2 titles fill 8 GB cards): let the driver page
        // it out rather than failing the draw and aborting.
        static u32 logged{};
        if (logged++ < 32) {
            LOG_WARNING(Render_Vulkan,
                        "Image {}x{}x{} ({} levels, {} layers, format {}) is over the memory "
                        "budget; allocating it anyway",
                        ci.extent.width, ci.extent.height, ci.extent.depth, ci.mipLevels,
                        ci.arrayLayers, static_cast<u32>(ci.format));
        }
        static std::atomic<u32> detailed{};
        const bool detail = detailed.fetch_add(1, std::memory_order_relaxed) < RefusalDetailEvents;
        if (detail) {
            const char* const first_try = kill_pooled                       ? "the colour target pool"
                                          : first_ci.pool != VK_NULL_HANDLE ? "the depth attachment pool"
                                                                            : "the default heaps";
            LOG_WARNING(Render_Vulkan,
                        "  refused: {} memory, first try in {}, VMA follows the driver budget: "
                        "{}; {}",
                        dedicated ? "dedicated" : "block", first_try,
                        device.CanReportMemoryUsage() ? "yes" : "no (no VK_EXT_memory_budget)",
                        HeapBudgetsText(allocator));
        }
        pressure_events.fetch_add(1, std::memory_order_relaxed);
        VmaAllocationCreateInfo relaxed = alloc_ci;
        relaxed.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        result = vmaCreateImage(allocator, &ci, &relaxed, &handle, &allocation, &alloc_info);
        if (detail) {
            LOG_WARNING(Render_Vulkan, "  without the budget check: {}",
                        result == VK_SUCCESS
                            ? "placed in " + PlacementText(allocator, alloc_info.memoryType)
                            : fmt::format("failed ({})", static_cast<int>(result)));
        }
    }
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY && !huge) {
        // Still out of device memory: a real image in host memory (slow, but valid) beats one
        // with nothing bound, which AMD's drivers do not survive being sampled.
        VmaAllocationCreateInfo host = alloc_ci;
        host.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        host.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        host.preferredFlags = 0;
        result = vmaCreateImage(allocator, &ci, &host, &handle, &allocation, &alloc_info);
        static u32 host_logged{};
        if (result == VK_SUCCESS && host_logged++ < 16) {
            LOG_WARNING(Render_Vulkan,
                        "Image {}x{}x{} ({} levels, {} layers, format {}) is out of device memory; "
                        "placed in host memory, {}",
                        ci.extent.width, ci.extent.height, ci.extent.depth, ci.mipLevels,
                        ci.arrayLayers, static_cast<u32>(ci.format),
                        PlacementText(allocator, alloc_info.memoryType));
        }
    }
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY && !huge) {
        pressure_events.fetch_add(1, std::memory_order_relaxed);
    }
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY && ci.imageType != VK_IMAGE_TYPE_1D &&
        UsesUnbackedFallback(texels, bytes, may_fail)) {
        // Too big to back at all, e.g. title 1's sparse virtual texture arrays (8192x8192,
        // 68 layers, ~23 GB): on the console only resident tiles have memory. Use an image with
        // nothing bound, so views and copies still work, instead of aborting. A smaller image
        // gets one only for a caller that would throw otherwise.
        if (vk::Image sparse = CreateUnbackedImage(ci)) {
            LOG_WARNING(Render_Vulkan,
                        "Image {}x{}x{} ({} levels, {} layers, format {}) does not fit; using an "
                        "unbacked sparse image",
                        ci.extent.width, ci.extent.height, ci.extent.depth, ci.mipLevels,
                        ci.arrayLayers, static_cast<u32>(ci.format));
            image = std::move(sparse);
            return VK_SUCCESS;
        }
    }
    if (result != VK_SUCCESS) {
        failed_images.fetch_add(1, std::memory_order_relaxed);
        static u32 failed_logged{};
        if (failed_logged == 0) {
            // What ran out: the device's heaps or the host's commit. Before the error line,
            // which is the one that reaches the log file when the session ends here.
            LogMemoryTrace(device);
        }
        if (failed_logged++ < 32) {
            LOG_ERROR(Render_Vulkan, "Image {}x{}x{} ({} levels, {} layers, format {}) failed: {}",
                      ci.extent.width, ci.extent.height, ci.extent.depth, ci.mipLevels,
                      ci.arrayLayers, static_cast<u32>(ci.format), static_cast<int>(result));
        }
        return result;
    }

    // Log GPU memory allocation for images
    if (GPU::Logging::IsActive() &&
        Settings::values.gpu_log_memory_tracking.GetValue()) {
        GPU::Logging::GPULogger::GetInstance().LogMemoryAllocation(
            reinterpret_cast<uintptr_t>(alloc_info.deviceMemory),
            static_cast<u64>(alloc_info.size),
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        );
    }

    NoteKillCensus(allocator, ci, allocation, Common::Nx2Hitch::NowNs() - kill_start, kill_pooled,
                   kill_candidate, kill_mode);

    image = vk::Image(handle, ci.usage, *device.GetLogical(), allocator, allocation,
                      device.GetDispatchLoader());
    return VK_SUCCESS;
}

vk::Image MemoryAllocator::CreateUnbackedImage(const VkImageCreateInfo &ci, bool aliased) const {
    if (ci.imageType == VK_IMAGE_TYPE_1D) {
        return vk::Image{};
    }
    VkImageCreateInfo sparse_ci = ci;
    sparse_ci.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
    if (aliased) {
        sparse_ci.flags |= VK_IMAGE_CREATE_SPARSE_ALIASED_BIT;
    }
    VkImage sparse{};
    if (device.GetDispatchLoader().vkCreateImage(*device.GetLogical(), &sparse_ci, nullptr,
                                                 &sparse) != VK_SUCCESS) {
        return vk::Image{};
    }
    return vk::Image(sparse, ci.usage, *device.GetLogical(), allocator, VK_NULL_HANDLE,
                     device.GetDispatchLoader());
}

VmaAllocation MemoryAllocator::AllocateSparseMemory(const VkMemoryRequirements &requirements,
                                                   VkDeviceMemory &memory,
                                                   VkDeviceSize &offset) const {
    const VideoCommon::MemoryStageLink::AllocatorOp allocator_op{allocator};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = 0,
        .usage = VMA_MEMORY_USAGE_UNKNOWN,
        .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        .preferredFlags = 0,
        .memoryTypeBits = requirements.memoryTypeBits,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
        .priority = 0.f,
    };
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    // In a pool of their own (see Device::SeparatePool); anything over half a block, or a pool
    // allocation that fails, takes the default blocks.
    u32 memory_type{};
    if (requirements.size <= Device::SeparatePoolBlockSize / 2 &&
        vmaFindMemoryTypeIndex(allocator, requirements.memoryTypeBits, &alloc_ci, &memory_type) ==
            VK_SUCCESS) {
        if (const VmaPool pool =
                device.GetSeparatePool(allocator, Device::SeparatePool::SparsePages, memory_type)) {
            VmaAllocationCreateInfo pool_ci = alloc_ci;
            pool_ci.pool = pool;
            if (vmaAllocateMemory(allocator, &requirements, &pool_ci, &allocation, &info) ==
                VK_SUCCESS) {
                memory = info.deviceMemory;
                offset = info.offset;
                return allocation;
            }
        }
    }
    if (vmaAllocateMemory(allocator, &requirements, &alloc_ci, &allocation, &info) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    memory = info.deviceMemory;
    offset = info.offset;
    return allocation;
}

void MemoryAllocator::FreeSparseMemory(VmaAllocation allocation) const {
    if (allocation != VK_NULL_HANDLE) {
        const VideoCommon::MemoryStageLink::AllocatorOp allocator_op{allocator};
        vmaFreeMemory(allocator, allocation);
    }
}

bool MemoryAllocator::CreateBufferInSeparatePool(const VkBufferCreateInfo& ci, MemoryUsage usage,
                                                 const VmaAllocationCreateInfo& alloc_ci,
                                                 VkBuffer& handle, VmaAllocation& allocation,
                                                 VmaAllocationInfo& alloc_info) const {
    // The buffer cache's buffers, churned by its collector: in a pool of their own (see
    // Device::SeparatePool). Anything over half a block takes the default blocks (dedicated).
    if (usage != MemoryUsage::DeviceLocal || ci.size > Device::SeparatePoolBlockSize / 2) {
        return false;
    }
    u32 memory_type{};
    {
        std::scoped_lock lk{buffer_type_mutex};
        const auto key = (u64{ci.usage} << 32) | ci.flags;
        if (const auto it = buffer_memory_types.find(key); it != buffer_memory_types.end()) {
            memory_type = it->second;
        } else if (vmaFindMemoryTypeIndexForBufferInfo(allocator, &ci, &alloc_ci, &memory_type) ==
                   VK_SUCCESS) {
            buffer_memory_types.emplace(key, memory_type);
        } else {
            return false;
        }
    }
    const VmaPool pool =
        device.GetSeparatePool(allocator, Device::SeparatePool::DeviceBuffers, memory_type);
    if (pool == VK_NULL_HANDLE) {
        return false;
    }
    VmaAllocationCreateInfo pool_ci = alloc_ci;
    pool_ci.pool = pool;
    pool_ci.memoryTypeBits = 0;
    return vmaCreateBuffer(allocator, &ci, &pool_ci, &handle, &allocation, &alloc_info) ==
           VK_SUCCESS;
}

vk::Buffer MemoryAllocator::CreateBuffer(const VkBufferCreateInfo &ci, MemoryUsage usage) const {
    const VideoCommon::MemoryStageLink::AllocatorOp allocator_op{allocator};
    // MESA will do memcpy() if not marked as host cached, so just force mark it for most buffers
    auto const anv_flags = (usage == MemoryUsage::Stream
        && device.GetDriverID() == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA)
        ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0;
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | MemoryUsageVmaFlags(usage),
        .usage = MemoryUsageVma(usage),
        .requiredFlags = 0,
        .preferredFlags = MemoryUsagePreferredVmaFlags(usage) | anv_flags,
        .memoryTypeBits = usage == MemoryUsage::Stream ? 0u : valid_memory_types,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
        .priority = 0.f,
    };

    VkBuffer handle{};
    VmaAllocationInfo alloc_info{};
    VmaAllocation allocation{};
    VkMemoryPropertyFlags property_flags{};

    if (!CreateBufferInSeparatePool(ci, usage, alloc_ci, handle, allocation, alloc_info)) {
        VkResult result = vmaCreateBuffer(allocator, &ci, &alloc_ci, &handle, &allocation,
                                          &alloc_info);
        // Staging buffers live in system memory, where the budget is the OS's soft hint: going
        // over it pages, refusing aborted the emulator (a texture upload in Ocarina's town
        // square after 36 minutes). Device-local buffers keep the budget.
        if ((result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_OUT_OF_HOST_MEMORY) &&
            usage != MemoryUsage::DeviceLocal) {
            VmaAllocationCreateInfo over_budget = alloc_ci;
            over_budget.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
            result = vmaCreateBuffer(allocator, &ci, &over_budget, &handle, &allocation,
                                     &alloc_info);
            LOG_WARNING(Render_Vulkan,
                        "Buffer of {} KiB (usage {}) over the memory budget: {}", ci.size >> 10,
                        static_cast<u32>(usage), result == VK_SUCCESS ? "made anyway" : "failed");
            static std::atomic<u32> detailed{};
            if (detailed.fetch_add(1, std::memory_order_relaxed) < RefusalDetailEvents) {
                LOG_WARNING(Render_Vulkan, "  {}; {}",
                            result == VK_SUCCESS
                                ? "placed in " + PlacementText(allocator, alloc_info.memoryType)
                                : std::string{"not placed"},
                            HeapBudgetsText(allocator));
            }
        }
        vk::Check(result);
    }
    vmaGetAllocationMemoryProperties(allocator, allocation, &property_flags);

    // Log GPU memory allocation for buffers
    if (GPU::Logging::IsActive() &&
        Settings::values.gpu_log_memory_tracking.GetValue()) {
        GPU::Logging::GPULogger::GetInstance().LogMemoryAllocation(
            reinterpret_cast<uintptr_t>(alloc_info.deviceMemory),
            static_cast<u64>(alloc_info.size),
            property_flags
        );
    }

    u8 *data = reinterpret_cast<u8 *>(alloc_info.pMappedData);
    const std::span<u8> mapped_data = data ? std::span<u8>{data, ci.size} : std::span<u8>{};
    const bool is_coherent = (property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

    const vk::MemoryLocation location{
        .memory = alloc_info.deviceMemory,
        .offset = alloc_info.offset,
        .memory_type = alloc_info.memoryType,
    };
    return vk::Buffer(handle, *device.GetLogical(), allocator, allocation, mapped_data, is_coherent,
                      location, device.GetDispatchLoader());
}

vk::Buffer MemoryAllocator::CreateBuffer(const VkBufferCreateInfo &ci, MemoryUsage usage,
                                         VkDeviceSize min_alignment) const {
    if (min_alignment <= 1) {
        return CreateBuffer(ci, usage);
    }
    const VideoCommon::MemoryStageLink::AllocatorOp allocator_op{allocator};
    VkMemoryPropertyFlags anv_flags = 0;
    if (usage == MemoryUsage::Stream &&
        device.GetDriverID() == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA) {
        anv_flags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    }
    u32 memory_type_bits = valid_memory_types;
    if (usage == MemoryUsage::Stream) {
        memory_type_bits = 0u;
    }
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | MemoryUsageVmaFlags(usage),
        .usage = MemoryUsageVma(usage),
        .requiredFlags = 0,
        .preferredFlags = MemoryUsagePreferredVmaFlags(usage) | anv_flags,
        .memoryTypeBits = memory_type_bits,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
        .priority = 0.f,
    };

    VkBuffer handle{};
    VmaAllocationInfo alloc_info{};
    VmaAllocation allocation{};
    VkMemoryPropertyFlags property_flags{};

    vk::Check(vmaCreateBufferWithAlignment(allocator, &ci, &alloc_ci, min_alignment, &handle,
                                           &allocation, &alloc_info));
    vmaGetAllocationMemoryProperties(allocator, allocation, &property_flags);

    u8 *data = reinterpret_cast<u8 *>(alloc_info.pMappedData);
    std::span<u8> mapped_data{};
    if (data) {
        mapped_data = std::span<u8>{data, ci.size};
    }
    const bool is_coherent = (property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

    const vk::MemoryLocation location{
        .memory = alloc_info.deviceMemory,
        .offset = alloc_info.offset,
        .memory_type = alloc_info.memoryType,
    };
    return vk::Buffer(handle, *device.GetLogical(), allocator, allocation, mapped_data, is_coherent,
                      location, device.GetDispatchLoader());
}

MemoryCommit MemoryAllocator::Commit(const VkMemoryRequirements &reqs, MemoryUsage usage)
{
    const VideoCommon::MemoryStageLink::AllocatorOp allocator_op{allocator};
    const auto vma_usage = MemoryUsageVma(usage);
    VmaAllocationCreateInfo ci{};
    ci.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | MemoryUsageVmaFlags(usage);
    ci.usage = vma_usage;
    ci.memoryTypeBits = reqs.memoryTypeBits & valid_memory_types;
    ci.requiredFlags = 0;
    ci.preferredFlags = MemoryUsagePreferredVmaFlags(usage);

    VmaAllocation a{};
    VmaAllocationInfo info{};

    VkResult res = vmaAllocateMemory(allocator, &reqs, &ci, &a, &info);

    if (res != VK_SUCCESS) {
        // Relax 1: drop budget constraint
        auto ci2 = ci;
        ci2.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        res = vmaAllocateMemory(allocator, &reqs, &ci2, &a, &info);

        // Relax 2: if we preferred DEVICE_LOCAL, drop that preference
        if (res != VK_SUCCESS && (ci.preferredFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            auto ci3 = ci2;
            ci3.preferredFlags &= ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            res = vmaAllocateMemory(allocator, &reqs, &ci3, &a, &info);
        }
    }

    vk::Check(res);
    return MemoryCommit(allocator, a, info);
}

MemoryCommit MemoryAllocator::Commit(const vk::Buffer &buffer, MemoryUsage usage) {
    const VideoCommon::MemoryStageLink::AllocatorOp allocator_op{allocator};
    // Allocate memory appropriate for this buffer automatically
    const auto vma_usage = MemoryUsageVma(usage);

    VmaAllocationCreateInfo ci{};
    ci.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | MemoryUsageVmaFlags(usage);
    ci.usage = vma_usage;
    ci.requiredFlags = 0;
    ci.preferredFlags = MemoryUsagePreferredVmaFlags(usage);
    ci.pool = VK_NULL_HANDLE;
    ci.pUserData = nullptr;
    ci.priority = 0.0f;

    const VkBuffer raw = *buffer;

    VmaAllocation a{};
    VmaAllocationInfo info{};

    // Let VMA infer memory requirements from the buffer
    VkResult res = vmaAllocateMemoryForBuffer(allocator, raw, &ci, &a, &info);

    if (res != VK_SUCCESS) {
        auto ci2 = ci;
        ci2.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        res = vmaAllocateMemoryForBuffer(allocator, raw, &ci2, &a, &info);

        if (res != VK_SUCCESS && (ci.preferredFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            auto ci3 = ci2;
            ci3.preferredFlags &= ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            res = vmaAllocateMemoryForBuffer(allocator, raw, &ci3, &a, &info);
        }
    }

    vk::Check(res);
    vk::Check(vmaBindBufferMemory2(allocator, a, 0, raw, nullptr));
    return MemoryCommit(allocator, a, info);
}

#ifdef _WIN32
namespace {
/// The DXGI adapter of the Vulkan device (matched by LUID), or null. Looked up again when the
/// device changes (another GPU picked between sessions).
IDXGIAdapter3* FindDxgiAdapter(const Device& device) {
    VkPhysicalDeviceIDProperties id{};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &id;
    device.GetPhysical().GetProperties2(properties);
    if (!id.deviceLUIDValid) {
        return nullptr;
    }
    LUID luid{};
    std::memcpy(&luid, id.deviceLUID, sizeof(luid));
    static LUID cached_luid{};
    static IDXGIAdapter3* cached{};
    if (cached != nullptr && cached_luid.LowPart == luid.LowPart &&
        cached_luid.HighPart == luid.HighPart) {
        return cached;
    }
    if (cached != nullptr) {
        cached->Release();
        cached = nullptr;
    }
    cached_luid = luid;
    const HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (dxgi == nullptr) {
        return nullptr;
    }
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    const auto create =
        reinterpret_cast<CreateFactory>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
    IDXGIFactory4* factory{};
    if (create == nullptr ||
        FAILED(create(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory)))) {
        return nullptr;
    }
    if (FAILED(factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter3),
                                          reinterpret_cast<void**>(&cached)))) {
        cached = nullptr;
    }
    factory->Release();
    return cached;
}
} // Anonymous namespace
#endif

void LogMemoryTrace(const Device& device) {
    const Common::ProcessMemoryUsage process = Common::GetProcessMemoryUsage();
    LOG_INFO(Render_Vulkan,
             "Memory trace: process commit {} MiB, working set {} MiB (private {} MiB), system "
             "commit {} of {} MiB",
             process.private_bytes >> 20, process.working_set >> 20,
             process.private_working_set >> 20,
             process.system_commit >> 20, process.system_commit_limit >> 20);
#ifdef _WIN32
    // This process's use of the card's memory (local) and of system memory for the GPU
    // (non-local: host memory types, and what the OS moved out of video memory).
    IDXGIAdapter3* const adapter = FindDxgiAdapter(device);
    DXGI_QUERY_VIDEO_MEMORY_INFO local{};
    DXGI_QUERY_VIDEO_MEMORY_INFO non_local{};
    if (adapter != nullptr &&
        SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
        SUCCEEDED(
            adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &non_local))) {
        LOG_INFO(Render_Vulkan,
                 "Memory trace: DXGI local {} of {} MiB, non-local {} of {} MiB",
                 local.CurrentUsage >> 20, local.Budget >> 20, non_local.CurrentUsage >> 20,
                 non_local.Budget >> 20);
    }
#endif
    const VmaAllocator allocator = device.GetAllocator();
    const VkPhysicalDeviceMemoryProperties* properties{};
    vmaGetMemoryProperties(allocator, &properties);
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
    vmaGetHeapBudgets(allocator, budgets.data());
    std::string heaps;
    for (u32 heap = 0; heap < properties->memoryHeapCount; ++heap) {
        const VmaBudget& budget = budgets[heap];
        fmt::format_to(std::back_inserter(heaps), "{}{} ({}): driver {} of {} MiB, VMA {}/{} MiB",
                       heap == 0 ? "" : "; ", heap,
                       (properties->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0
                           ? "local"
                           : "host",
                       budget.usage >> 20, budget.budget >> 20,
                       budget.statistics.allocationBytes >> 20,
                       budget.statistics.blockBytes >> 20);
    }
    LOG_INFO(Render_Vulkan, "Memory trace: heaps {}", heaps);
    // The heaps' VMA numbers above are the main allocator's; with FRITCHII_MEMSTAGE_SPLIT_ALLOC
    // the buffer allocator's memory types get a line of their own.
    const auto log_types = [&](VmaAllocator owner, const char* label) {
        VmaTotalStatistics stats{};
        vmaCalculateStatistics(owner, &stats);
        std::string types;
        for (u32 type = 0; type < properties->memoryTypeCount; ++type) {
            const VmaStatistics& s = stats.memoryType[type].statistics;
            if (s.blockCount == 0) {
                continue;
            }
            fmt::format_to(std::back_inserter(types),
                           "{}{} (heap {}, flags {:#x}): {} allocs {}/{} MiB",
                           types.empty() ? "" : "; ", type, properties->memoryTypes[type].heapIndex,
                           static_cast<u32>(properties->memoryTypes[type].propertyFlags),
                           s.allocationCount, s.allocationBytes >> 20, s.blockBytes >> 20);
        }
        LOG_INFO(Render_Vulkan, "Memory trace: {} {}", label, types);
    };
    log_types(allocator, "types");
    // Only its user may walk the buffer allocator (memory stage spec 5.3).
    if (device.HasSplitAllocators() && VideoCommon::MemoryStageLink::IsCacheUser()) {
        log_types(device.GetBufferAllocator(), "buffer allocator types");
    }
}

HeapTotals GetHeapTotals(const Device& device) {
    const VmaAllocator allocator = device.GetAllocator();
    const VkPhysicalDeviceMemoryProperties* properties{};
    vmaGetMemoryProperties(allocator, &properties);
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
    vmaGetHeapBudgets(allocator, budgets.data());
    HeapTotals totals{};
    for (u32 heap = 0; heap < properties->memoryHeapCount; ++heap) {
        const VmaBudget& budget = budgets[heap];
        if ((properties->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
            totals.local_usage += budget.usage;
            totals.local_budget += budget.budget;
            totals.local_blocks += budget.statistics.blockBytes;
        } else {
            totals.host_usage += budget.usage;
            totals.host_budget += budget.budget;
            totals.host_blocks += budget.statistics.blockBytes;
        }
    }
    if (device.HasSplitAllocators()) {
        // FRITCHII_MEMSTAGE_SPLIT_ALLOC: the buffer allocator's blocks too. Its usage only
        // without VK_EXT_memory_budget: with it, usage is the driver's for the whole process.
        // At FRITCHII_MEMSTAGE_THREAD=1 as its user published them (memory stage spec 5.3).
        device.BufferHeapBudgets(budgets.data());
        const bool add_usage = !device.CanReportMemoryUsage();
        for (u32 heap = 0; heap < properties->memoryHeapCount; ++heap) {
            const VmaBudget& budget = budgets[heap];
            const u64 usage = add_usage ? budget.usage : 0;
            if ((properties->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
                totals.local_usage += usage;
                totals.local_blocks += budget.statistics.blockBytes;
            } else {
                totals.host_usage += usage;
                totals.host_blocks += budget.statistics.blockBytes;
            }
        }
    }
    return totals;
}

void LogMemoryStatistics(const Device& device) {
    const VmaAllocator allocator = device.GetAllocator();
    const VkPhysicalDeviceMemoryProperties* properties{};
    vmaGetMemoryProperties(allocator, &properties);
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(allocator, &stats);
    for (u32 type = 0; type < properties->memoryTypeCount; ++type) {
        const VmaStatistics& s = stats.memoryType[type].statistics;
        if (s.blockCount == 0) {
            continue;
        }
        LOG_DEBUG(Render_Vulkan, "VMA type {} (heap {}, flags {:#x}): {} blocks {} MiB, {} allocations {} MiB",
                 type, properties->memoryTypes[type].heapIndex,
                 static_cast<u32>(properties->memoryTypes[type].propertyFlags), s.blockCount,
                 s.blockBytes >> 20, s.allocationCount, s.allocationBytes >> 20);
    }
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
    vmaGetHeapBudgets(allocator, budgets.data());
    for (u32 heap = 0; heap < properties->memoryHeapCount; ++heap) {
        LOG_INFO(Render_Vulkan, "VMA heap {}: usage {} MiB of budget {} MiB, VMA blocks {} MiB",
                 heap, budgets[heap].usage >> 20, budgets[heap].budget >> 20,
                 budgets[heap].statistics.blockBytes >> 20);
    }
    if (device.HasSplitAllocators()) {
        device.BufferHeapBudgets(budgets.data());
        for (u32 heap = 0; heap < properties->memoryHeapCount; ++heap) {
            LOG_INFO(Render_Vulkan, "VMA heap {}: buffer allocator blocks {} MiB ({} MiB used)",
                     heap, budgets[heap].statistics.blockBytes >> 20,
                     budgets[heap].statistics.allocationBytes >> 20);
        }
    }
}

} // namespace Vulkan
