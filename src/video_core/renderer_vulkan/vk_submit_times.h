// SPDX-FileCopyrightText: Copyright 2026 Fritchii
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <memory>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class MasterSemaphore;

/// FRITCHII_SUBMIT_TIMES=1 (diagnostic, read at first use, default 0; helper_cu_answer.md Round
/// 3): a GPU timestamp at the start of every main-queue submission (top of its upload command
/// buffer) and at its end (bottom of its command buffer), read back lazily, when someone asks
/// about a run of ticks. The read-back counters ask, at each read's cut at its writer's tick
/// (FRITCHII_READ_RUNS=2), how the submissions the card had to run before the writer split into
/// GPU busy time and the gaps between them (submission and scheduling overhead).
class SubmitTimes {
public:
    /// What the submissions [first, last] took on the GPU.
    struct Span {
        u32 submissions{}; ///< asked for (capped at MaxSpan)
        u32 resolved{};    ///< with both timestamps read
        u64 busy_ns{};     ///< sum of end - start
        u64 gap_ns{};      ///< sum of the idle time between one's end and the next one's start
        u64 overlap_ns{};  ///< sum of the time a submission started before the previous ended
        u64 span_ns{};     ///< the first resolved one's start to the last one's end
    };

    static constexpr u32 Slots = 4096;
    static constexpr u32 MaxSpan = 64;

    [[nodiscard]] static bool Enabled();

    /// The worker, after the upload command buffer began: the start stamp of the next submission.
    void Begin(const Device& device, vk::CommandBuffer upload_cmdbuf);
    /// The worker, before the command buffer ends: its end stamp, for scheduler tick `tick`.
    void End(vk::CommandBuffer cmdbuf, u64 tick);

    /// Any thread: the submissions [first, last], those the host GPU completed (`master`).
    [[nodiscard]] Span Measure(u64 first, u64 last, MasterSemaphore& master) const;

private:
    bool ReadSlot(u32 slot, u64& start_ns, u64& end_ns) const;

    const Device* device{};
    vk::QueryPool pool;
    double period_ns{};
    u64 mask{};
    bool failed{};
    bool recording{};
    u32 slot{};      ///< the slot the submission being recorded uses
    u64 sequence{};  ///< submissions stamped so far
    /// Per slot: the tick it holds (0: none yet). Per tick % Slots: the slot.
    std::unique_ptr<std::atomic<u64>[]> slot_tick{new std::atomic<u64>[Slots]{}};
    std::unique_ptr<std::atomic<u32>[]> tick_slot{new std::atomic<u32>[Slots]{}};
};

} // namespace Vulkan
