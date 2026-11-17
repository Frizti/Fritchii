// SPDX-FileCopyrightText: Copyright 2026 Fritchii
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include "common/logging.h"
#include "video_core/early_compute.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_submit_times.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

bool SubmitTimes::Enabled() {
    return static_cast<bool>(VideoCommon::EarlyCompute::g_submit_times);
}

void SubmitTimes::Begin(const Device& device_, vk::CommandBuffer upload_cmdbuf) {
    recording = false;
    if (failed) {
        return;
    }
    if (!pool) {
        device = &device_;
        const auto physical = device_.GetPhysical();
        const u32 bits =
            physical.GetQueueFamilyProperties()[device_.GetGraphicsFamily()].timestampValidBits;
        period_ns = physical.GetProperties().limits.timestampPeriod;
        if (bits == 0 || bits > 64 || period_ns <= 0) {
            LOG_INFO(Render_Vulkan, "FRITCHII_SUBMIT_TIMES: the graphics queue has no timestamps");
            failed = true;
            return;
        }
        mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
        try {
            pool = device_.GetLogical().CreateQueryPool({
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                .queryCount = Slots * 2,
                .pipelineStatistics = 0,
            });
        } catch (const vk::Exception& error) {
            LOG_WARNING(Render_Vulkan, "FRITCHII_SUBMIT_TIMES unavailable: {}", error.what());
            failed = true;
            return;
        }
        LOG_INFO(Render_Vulkan, "FRITCHII_SUBMIT_TIMES: a GPU timestamp at the start and end of "
                                "every submission ({} slots)",
                 Slots);
    }
    slot = static_cast<u32>(sequence % Slots);
    // Not a tick of this slot any more: its queries are about to be reset.
    slot_tick[slot].store(0, std::memory_order_release);
    upload_cmdbuf.ResetQueryPool(*pool, slot * 2, 2);
    upload_cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, *pool, slot * 2);
    recording = true;
}

void SubmitTimes::End(vk::CommandBuffer cmdbuf, u64 tick) {
    if (!recording) {
        return;
    }
    recording = false;
    cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *pool, slot * 2 + 1);
    tick_slot[tick % Slots].store(slot, std::memory_order_relaxed);
    slot_tick[slot].store(tick, std::memory_order_release);
    ++sequence;
}

bool SubmitTimes::ReadSlot(u32 index, u64& start_ns, u64& end_ns) const {
    std::array<u64, 2> stamps{};
    const VkResult result = device->GetLogical().GetQueryResults(
        *pool, index * 2, 2, sizeof(stamps), stamps.data(), sizeof(u64), VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS) {
        return false;
    }
    start_ns = static_cast<u64>(static_cast<double>(stamps[0] & mask) * period_ns);
    end_ns = static_cast<u64>(static_cast<double>(stamps[1] & mask) * period_ns);
    return end_ns >= start_ns;
}

SubmitTimes::Span SubmitTimes::Measure(u64 first, u64 last, MasterSemaphore& master) const {
    Span span{};
    if (!pool || failed || first == 0 || last < first) {
        return span;
    }
    // The newest ones count: a long run's oldest submissions had mostly run at the read.
    if (last - first + 1 > MaxSpan) {
        first = last - MaxSpan + 1;
    }
    master.Refresh();
    u64 previous_end = 0;
    u64 first_start = 0;
    for (u64 tick = first; tick <= last; ++tick) {
        ++span.submissions;
        if (!master.IsFree(tick)) {
            previous_end = 0;
            continue;
        }
        const u32 index = tick_slot[tick % Slots].load(std::memory_order_relaxed);
        if (slot_tick[index].load(std::memory_order_acquire) != tick) {
            previous_end = 0;
            continue;
        }
        u64 start = 0;
        u64 end = 0;
        // Read, then make sure the slot still held this tick (not reset meanwhile).
        if (!ReadSlot(index, start, end) ||
            slot_tick[index].load(std::memory_order_acquire) != tick) {
            previous_end = 0;
            continue;
        }
        ++span.resolved;
        span.busy_ns += end - start;
        if (previous_end != 0) {
            if (start >= previous_end) {
                span.gap_ns += start - previous_end;
            } else {
                span.overlap_ns += previous_end - start;
            }
        }
        if (first_start == 0) {
            first_start = start;
        }
        span.span_ns = end > first_start ? end - first_start : 0;
        previous_end = end;
    }
    return span;
}

} // namespace Vulkan
