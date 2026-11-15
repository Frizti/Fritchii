// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <span>

#include "common/common_types.h"
#include "video_core/dirty_flags.h"

namespace VideoCommon {

// Ported from zortos293/Lichii#1459 (1207061d384694bdcf2b11ebd9f66e478c425651),
// based on Eglantine c6d2acd37a and PR #503. Resolved IDs survive channel switches:
// clear them before a deleted slot can be reused for an unrelated buffer.
template <bool PersistentUniformBindings, typename Channel, typename Storage, typename Id,
          typename UpdateVertex>
void InvalidateDeletedBufferBindings(Channel& current, Storage& channels,
                                     std::span<const size_t> active_channels, Id deleted,
                                     UpdateVertex update_vertex) {
    const auto scalar_replace = [deleted](auto& binding) {
        if (binding.buffer_id == deleted) {
            binding.buffer_id = {};
        }
    };
    const auto replace = [scalar_replace](auto& bindings) {
        std::ranges::for_each(bindings, scalar_replace);
    };
    const auto scrub = [&](Channel& channel) {
        auto& flags = channel.maxwell3d.dirty.flags;
        if (channel.index_buffer.buffer_id == deleted) {
            channel.index_buffer.buffer_id = {};
            flags[Dirty::IndexBuffer] = true;
        }
        for (u32 index = 0; index < channel.vertex_buffers.size(); ++index) {
            auto& binding = channel.vertex_buffers[index];
            if (binding.buffer_id != deleted) {
                continue;
            }
            binding.buffer_id = {};
            if (&channel == &current) {
                update_vertex(index, binding);
            }
            flags[Dirty::VertexBuffers] = true;
            flags[Dirty::VertexBuffer0 + index] = true;
        }
        std::ranges::for_each(channel.uniform_buffers, replace);
        std::ranges::for_each(channel.storage_buffers, replace);
        std::ranges::for_each(channel.texture_buffers, replace);
        replace(channel.transform_feedback_buffers);
        scalar_replace(channel.count_buffer_binding);
        scalar_replace(channel.indirect_buffer_binding);
        replace(channel.compute_uniform_buffers);
        replace(channel.compute_storage_buffers);
        replace(channel.compute_texture_buffers);
        if constexpr (PersistentUniformBindings) {
            channel.dirty_uniform_buffers.fill(~u32{0});
            channel.uniform_buffer_binding_sizes.fill({});
        }
        channel.has_deleted_buffers = true;
    };
    scrub(current);
    for (const size_t id : active_channels) {
        if (&channels[id] != &current) {
            scrub(channels[id]);
        }
    }
}

} // namespace VideoCommon
