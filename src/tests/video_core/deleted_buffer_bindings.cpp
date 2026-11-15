// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/slot_vector.h"
#include "video_core/buffer_cache/buffer_cache_base.h"
#include "video_core/buffer_cache/deleted_buffer_bindings.h"

namespace {

using VideoCommon::Binding;
using VideoCommon::BufferCacheChannelInfo;
using VideoCommon::BufferId;
namespace Dirty = VideoCommon::Dirty;

// Use the production binding types without constructing guest engines or a Vulkan device.
struct Channel {
    struct {
        struct {
            Tegra::Engines::Maxwell3D::DirtyState::Flags flags;
        } dirty;
    } maxwell3d;
    decltype(BufferCacheChannelInfo::index_buffer) index_buffer{};
    decltype(BufferCacheChannelInfo::vertex_buffers) vertex_buffers{};
    decltype(BufferCacheChannelInfo::uniform_buffers) uniform_buffers{};
    decltype(BufferCacheChannelInfo::storage_buffers) storage_buffers{};
    decltype(BufferCacheChannelInfo::texture_buffers) texture_buffers{};
    decltype(BufferCacheChannelInfo::transform_feedback_buffers) transform_feedback_buffers{};
    decltype(BufferCacheChannelInfo::count_buffer_binding) count_buffer_binding{};
    decltype(BufferCacheChannelInfo::indirect_buffer_binding) indirect_buffer_binding{};
    decltype(BufferCacheChannelInfo::compute_uniform_buffers) compute_uniform_buffers{};
    decltype(BufferCacheChannelInfo::compute_storage_buffers) compute_storage_buffers{};
    decltype(BufferCacheChannelInfo::compute_texture_buffers) compute_texture_buffers{};
    decltype(BufferCacheChannelInfo::dirty_uniform_buffers) dirty_uniform_buffers{};
    decltype(BufferCacheChannelInfo::uniform_buffer_binding_sizes) uniform_buffer_binding_sizes{};
    bool has_deleted_buffers{};
};

std::array<Binding*, 11> Bindings(Channel& channel) {
    return {&channel.index_buffer,
            &channel.vertex_buffers[2],
            &channel.uniform_buffers[0][0],
            &channel.storage_buffers[4][1],
            &channel.texture_buffers[3][2],
            &channel.transform_feedback_buffers[3],
            &channel.count_buffer_binding,
            &channel.indirect_buffer_binding,
            &channel.compute_uniform_buffers[5],
            &channel.compute_storage_buffers[1],
            &channel.compute_texture_buffers[2]};
}

} // namespace

TEST_CASE("Deleted buffer slots cannot survive in another channel's bindings",
          "[video_core][deleted_buffer_bindings]") {
    // Regression for Lichii #1459: deleting on channel 0 must invalidate channel 1 before reuse.
    Common::SlotVector<u64> buffers;
    const auto null_id = buffers.insert(0);
    REQUIRE(null_id == VideoCommon::NULL_BUFFER_ID);
    const auto deleted = buffers.insert(0x10000);
    const auto unrelated = buffers.insert(0x50000);
    std::array<Channel, 3> channels{};
    const std::array<size_t, 2> active{0, 1};
    const Binding bound{.device_addr = 0x10800,
                        .gpu_addr = 0x20800,
                        .size = 0x100,
                        .buffer_id = deleted};
    for (auto& channel : channels) {
        for (auto* binding : Bindings(channel)) {
            *binding = bound;
        }
        channel.vertex_buffers[1].buffer_id = unrelated;
        channel.uniform_buffers[0][1].buffer_id = unrelated;
        channel.uniform_buffers[1][0].buffer_id = null_id;
        channel.dirty_uniform_buffers.fill(0x4);
        channel.uniform_buffer_binding_sizes.fill({0x80});
        channel.maxwell3d.dirty.flags[Dirty::RenderTargets] = true;
    }
    std::vector<u32> host_vertex_updates;
    auto update_vertex = [&](u32 index, const Binding& binding) {
        REQUIRE(&binding == &channels[0].vertex_buffers[index]);
        REQUIRE_FALSE(binding.buffer_id);
        host_vertex_updates.push_back(index);
    };
    bool persistent{};
    SECTION("Persistent uniform bindings are dirtied on every active channel") {
        persistent = true;
        VideoCommon::InvalidateDeletedBufferBindings<true>(channels[0], channels, active, deleted,
                                                          update_vertex);
    }
    SECTION("Nonpersistent uniform bindings retain their existing state") {
        VideoCommon::InvalidateDeletedBufferBindings<false>(channels[0], channels, active, deleted,
                                                           update_vertex);
    }
    REQUIRE(host_vertex_updates == std::vector<u32>{2});
    buffers.erase(deleted);
    REQUIRE(buffers.insert(0x30000) == deleted);
    REQUIRE(buffers[deleted] == 0x30000);

    for (const size_t id : active) {
        auto& channel = channels[id];
        for (const auto* binding : Bindings(channel)) {
            REQUIRE_FALSE(binding->buffer_id);
            REQUIRE(binding->device_addr == bound.device_addr);
            REQUIRE(binding->gpu_addr == bound.gpu_addr);
            REQUIRE(binding->size == bound.size);
        }
        REQUIRE(channel.vertex_buffers[1].buffer_id == unrelated);
        REQUIRE(channel.uniform_buffers[0][1].buffer_id == unrelated);
        REQUIRE(channel.uniform_buffers[1][0].buffer_id == null_id);
        REQUIRE(channel.has_deleted_buffers);
        const auto& flags = channel.maxwell3d.dirty.flags;
        REQUIRE(flags[Dirty::IndexBuffer]);
        REQUIRE(flags[Dirty::VertexBuffers]);
        REQUIRE(flags[Dirty::VertexBuffer0 + 2]);
        REQUIRE_FALSE(flags[Dirty::VertexBuffer0 + 1]);
        REQUIRE(flags[Dirty::RenderTargets]);
        for (const u32 mask : channel.dirty_uniform_buffers) {
            REQUIRE(mask == (persistent ? ~u32{0} : 0x4));
        }
        for (const auto& sizes : channel.uniform_buffer_binding_sizes) {
            REQUIRE(sizes[0] == (persistent ? 0 : 0x80));
        }
    }
    // Inactive storage may no longer hold a constructed channel; do not visit it.
    for (const auto* binding : Bindings(channels[2])) {
        REQUIRE(binding->buffer_id == deleted);
    }
    REQUIRE_FALSE(channels[2].has_deleted_buffers);
    REQUIRE_FALSE(channels[2].maxwell3d.dirty.flags[Dirty::IndexBuffer]);
}

TEST_CASE("Deleting a different buffer does not dirty index or vertex bindings",
          "[video_core][deleted_buffer_bindings]") {
    std::array<Channel, 2> channels{};
    const std::array<size_t, 2> active{0, 1};
    for (auto& channel : channels) {
        channel.index_buffer.buffer_id = BufferId{2};
        channel.vertex_buffers[0].buffer_id = BufferId{2};
    }
    VideoCommon::InvalidateDeletedBufferBindings<false>(
        channels[0], channels, active, BufferId{1},
        [](u32, const Binding&) { FAIL("Unchanged host vertex slot was updated"); });
    for (const auto& channel : channels) {
        REQUIRE(channel.index_buffer.buffer_id == BufferId{2});
        REQUIRE(channel.vertex_buffers[0].buffer_id == BufferId{2});
        REQUIRE(channel.maxwell3d.dirty.flags.none());
    }
}
