// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <span>

#include <boost/container/small_vector.hpp>

#include "common/common_types.h"
#include "common/thread_worker.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace VideoCore {
class ShaderNotify;
}

namespace Vulkan {

class Device;
class PipelineStatistics;
class Scheduler;

class ComputePipeline {
public:
    explicit ComputePipeline(const Device& device, Scheduler& scheduler, vk::PipelineCache& pipeline_cache,
                             DescriptorPool& descriptor_pool,
                             GuestDescriptorQueue& guest_descriptor_queue,
                             DescriptorBufferRing& descriptor_buffer_ring,
                             Common::ThreadWorker* thread_worker,
                             PipelineStatistics* pipeline_statistics,
                             VideoCore::ShaderNotify* shader_notify, const Shader::Info& info,
                             vk::ShaderModule spv_module, u64 shader_hash,
                             std::function<vk::ShaderModule()> make_module = {});

    ComputePipeline& operator=(ComputePipeline&&) noexcept = delete;
    ComputePipeline(ComputePipeline&&) noexcept = delete;

    ComputePipeline& operator=(const ComputePipeline&) = delete;
    ComputePipeline(const ComputePipeline&) = delete;

    using BindCommand = std::function<void(vk::CommandBuffer)>;

    [[nodiscard]] bool Configure(Tegra::Engines::KeplerCompute& kepler_compute,
                                 Tegra::MemoryManager& gpu_memory, Scheduler& scheduler,
                                 BufferCache& buffer_cache, TextureCache& texture_cache,
                                 TextureCacheRuntime& texture_runtime,
                                 std::optional<ByteAddressedShadow>& shadow,
                                 BindCommand* deferred_bind = nullptr);

    void FillHandleValues(MemStage& seam, const Tegra::Engines::KeplerCompute& kepler_compute,
                          Tegra::MemoryManager& gpu_memory, bool null_pointer_handles,
                          bool in_configure);

    /// One follower (FRITCHII_ONEF_OP_DISPATCH, E): the raw handles FillHandleValues makes, in
    /// the same order, into `values` (E's own vector; the seam's packet is not touched).
    void MakeHandleValues(const Tegra::Engines::KeplerCompute& kepler_compute,
                          Tegra::MemoryManager& gpu_memory, bool null_pointer_handles,
                          std::vector<u32>& values) const;
    /// One follower (E): whether the program writes an image (a storage image or an image
    /// buffer), from its info (WrittenImages is the last Configure's, R's).
    [[nodiscard]] bool WritesImages() const noexcept {
        return std::ranges::any_of(info.image_descriptors, [](const auto& desc) {
                   return desc.is_written;
               }) ||
               std::ranges::any_of(info.image_buffer_descriptors,
                                   [](const auto& desc) { return desc.is_written; });
    }
    /// One follower (FRITCHII_ONEF_DISPATCH_RANGES): calls `visit(raw)` for every written image
    /// element, in Configure's order (WrittenImages' order), from the values MakeHandleValues
    /// made (one value per image element, the image descriptors' last). False when there are too
    /// few values for that.
    template <typename Visit>
    bool VisitWrittenImageHandles(std::span<const u32> values, Visit&& visit) const {
        const size_t count = Shader::NumDescriptors(info.image_descriptors);
        if (values.size() < count) {
            return false;
        }
        size_t at = values.size() - count;
        for (const auto& desc : info.image_descriptors) {
            for (u32 element = 0; element < desc.count; ++element, ++at) {
                if (desc.is_written) {
                    visit(values[at]);
                }
            }
        }
        return true;
    }
    /// One follower (E): whether a written image is byte-addressed (Configure's packed R16
    /// shadow may apply: it throws on unsupported shapes and makes its own resources).
    [[nodiscard]] bool WritesByteAddressedImages() const noexcept {
        return std::ranges::any_of(info.image_descriptors, [](const auto& desc) {
            return desc.is_written && desc.is_byte_addressed;
        });
    }

    static constexpr u64 Ocarina1440pShader = 0x6bd913e1f34e83ebULL;
    /// FRITCHII_DLSS_RENDER_SCALE: the same upscale to 2560x1440 from a scene the DLSS render
    /// scale shrank (Ultra Performance, 1280x720), in menus and in gameplay.
    static constexpr u64 Ocarina1440pScaledShader = 0x4c1ae2fe58f30a97ULL;
    static constexpr u64 Ocarina1440pScaledFieldShader = 0x9302bc0d6d1d9bc1ULL;
    /// The game's 3/4 upscale, which it runs to 2560x1440 at the Performance render scale.
    static constexpr u64 OcarinaThreeQuarterShader = 0xe40f5efb5480e692ULL;
    /// The same output pass at 1:1, run at 1920x1080 in handheld with the Max Resolution add-on.
    static constexpr u64 OcarinaFullSizeShader = 0xc033c3ddc9f0897bULL;
    [[nodiscard]] bool CanReplaceOcarinaUpscale() const;

    bool IsBound() const noexcept {
        return static_cast<bool>(pipeline);
    }

    [[nodiscard]] u64 ShaderHash() const noexcept {
        return shader_hash;
    }

    /// What the memory stage's FillDispatch needs (SEAM = 2, vk_mem_stage.cpp).
    [[nodiscard]] MemStage::DispatchFacts MemFacts() const noexcept {
        return {
            .uniform_mask = info.constant_buffer_mask,
            .uniform_sizes = &uniform_buffer_sizes,
            .info = &info,
            .shader_hash = shader_hash,
            .has_texel_buffers =
                !info.texture_buffer_descriptors.empty() || !info.image_buffer_descriptors.empty(),
            .uses_descriptor_buffer = uses_descriptor_buffer,
        };
    }

    /// Storage images written by the last configured dispatch
    [[nodiscard]] std::span<const VideoCommon::ImageViewId> WrittenImages() const noexcept {
        return written_images;
    }

    struct SampledImage {
        u32 tic;
        VideoCommon::ImageViewId id;
        VideoCommon::SamplerId sampler;
    };

    /// The textures sampled by the last configured dispatch, kept while a sampled frame dump runs.
    [[nodiscard]] std::span<const SampledImage> SampledImages() const noexcept {
        return sampled_images;
    }

    /// Advances the frame window of EDEN_NX2_UPSCALER_TRACE
    static void TickUpscalerTrace();

    /// Made from a precompiled pipeline's kept info, its module from the kept SPIR-V
    /// (EDEN_NX2_PRECOMPILED_FAST): a failed build is replaced by a translated pipeline.
    void MarkPrecompiled() noexcept {
        precompiled = true;
    }
    [[nodiscard]] bool IsPrecompiled() const noexcept {
        return precompiled;
    }
    /// Whether the build ended without a pipeline (its module could not be made).
    [[nodiscard]] bool BuildFailed() const noexcept {
        return is_built.load(std::memory_order::acquire) && !pipeline;
    }
    /// Whether the build ended, with a pipeline or not.
    [[nodiscard]] bool IsBuilt() const noexcept {
        return is_built.load(std::memory_order::acquire);
    }

    /// Whether no build of this pipeline is left queued on a worker thread (the pipeline workers
    /// or FRITCHII_ASYNC_COMPUTE_SKIP's kick thread), which holds it
    [[nodiscard]] bool IsBuildTaskDone() const noexcept {
        return build_task_done.load(std::memory_order::acquire) &&
               !kick_pending.load(std::memory_order::acquire);
    }

    // --- FRITCHII_ASYNC_COMPUTE_SKIP (vk_async_compute_skip.h) ---

    /// Waits at most `timeout` for the build, never building it on the calling thread. True
    /// once built.
    [[nodiscard]] bool WaitBuiltFor(std::chrono::nanoseconds timeout);
    /// Starts the build on `worker` now, unless a thread claimed it already: the pipeline
    /// workers are first in, first out and may hold hundreds of builds before this one.
    void KickBuild(Common::ThreadWorker& worker);
    /// When the build ended (steady clock); only once IsBuilt().
    [[nodiscard]] std::chrono::steady_clock::time_point BuiltAt() const noexcept {
        return std::chrono::steady_clock::time_point{
            std::chrono::steady_clock::duration{built_at.load(std::memory_order::acquire)}};
    }
    /// Configure's storage pointer binds (the uniform state, the storage buffer binds and the
    /// shader hash), done ahead so the dispatch's written ranges can be judged before anything
    /// is uploaded or marked. The buffer cache's lock held. Configure skips them once after.
    void BindStoragePointers(BufferCache& buffer_cache);
    /// The pointers BindStoragePointers bound are the ones Configure uses next (set by the
    /// caller after BindStoragePointers, for a dispatch that runs).
    void UsePreboundPointers() noexcept {
        pointers_prebound = true;
    }
    /// FRITCHII_ASYNC_COMPUTE_SKIP_DELAY_MS (test): the next Configure records the build wait a
    /// pipeline still building records (on the scheduler worker, every channel behind it, the
    /// "Compute dispatch waited" line), held until `deadline`.
    void SetTestWaitUntil(std::chrono::steady_clock::time_point deadline) noexcept {
        test_wait_until = deadline;
    }

private:
    /// Builds the pipeline once, on whichever thread gets here first.
    void RunBuild();

    const Device& device;
    vk::PipelineCache& pipeline_cache;
    GuestDescriptorQueue& guest_descriptor_queue;
    DescriptorBufferRing& descriptor_buffer_ring;
    Shader::Info info;
    u64 shader_hash{};
    u32 num_descriptor_entries{};
    boost::container::small_vector<VideoCommon::ImageViewId, 8> written_images;
    std::vector<SampledImage> sampled_images;

    VideoCommon::ComputeUniformBufferSizes uniform_buffer_sizes{};

    vk::ShaderModule spv_module;
    vk::DescriptorSetLayout descriptor_set_layout;
    bool uses_push_descriptor{false};
    bool uses_descriptor_buffer{false};
    DescriptorBufferLayout descriptor_buffer_layout;
    DescriptorAllocator descriptor_allocator;
    vk::PipelineLayout pipeline_layout;
    vk::DescriptorUpdateTemplate descriptor_update_template;
    vk::Pipeline pipeline;

    std::function<void()> build_task;
    bool precompiled{};
    std::atomic_bool build_claimed{false};
    std::atomic_bool build_task_done{true};
    std::condition_variable build_condvar;
    std::mutex build_mutex;
    std::atomic_bool is_built{false};
    /// FRITCHII_ASYNC_COMPUTE_SKIP: the build's end (steady clock ticks), a kick queued, and
    /// Configure's pointer binds done ahead (BindStoragePointers).
    std::atomic<std::chrono::steady_clock::rep> built_at{};
    std::atomic_bool kick_pending{false};
    bool pointers_prebound{};
    std::chrono::steady_clock::time_point test_wait_until{};
};

} // namespace Vulkan
