// SPDX-FileCopyrightText: Copyright 2026 Fritchii
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// FRITCHII_ASYNC_COMPUTE_SKIP (live, default 0; docs/fritchii/switches.md). A native dispatch
// whose compute pipeline is not built yet recorded a wait on the Vulkan worker that every
// channel waits behind: 0.6-5.5 s freezes on cold shader caches (tester logs t0357, t0419, t1858;
// "Compute dispatch waited N ms for pipeline X"). With the switch on, such a dispatch is skipped
// instead, as if the guest had not run it this frame, when its outputs are per-frame results the
// GPU already holds and the guest never reads back:
//   G1 every byte it would mark written holds GPU writes (no CPU write to upload);
//   G2 each written range was written in each of the last 8 frames (buffers: this file's exact
//      (address, size) streaks; images: ImageBase::write_streak), or it is a range this pipeline
//      already skipped in the frame before (the chain: a skip writes nothing, so the streak it
//      passed at the first skip is kept, never extended);
//   G3 the guest never read a page of it back (Preflushable, cpu_read_ranges, read downloads, or
//      any guest read this file saw on its pages: sticky for the session);
//   G4 a plain native direct dispatch with known write extents (no indirect grid, no texel
//      buffers, no 16 MiB or capped marks, no global stores, no upscaler or effects pass).
// Otherwise it waits as before. 2 = dry: every dispatch waits, the line counts what 1 would skip.
// The decision comes before the dispatch's fill (MemStage::FillDispatch), so a skipped dispatch
// leaves no marks, uploads or records; its QMD releases are the engine's and still happen.
// Idea and rules from zortos293/Lichii #2104 (75fb0b527e, ffd0ebe246, ce937db2d0) by zortos293.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/container/small_vector.hpp>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/range_sets.h"
#include "common/thread_worker.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"

namespace Vulkan {

class AsyncComputeSkip {
public:
    using Clock = std::chrono::steady_clock;

    /// Why an unbuilt dispatch waited (the first that applies, in this order).
    enum class Refusal : u8 {
        Shape,         ///< G4: indirect, texel buffers, a packed R16 image, global stores, an
                       ///< OoT upscaler or effects pass, a frame dump, not native
        Extent,        ///< G4: a write without a known extent (16 MiB or capped mark, several
                       ///< device ranges, an image without one device range)
        ReadBack,      ///< G3
        NoHistory,     ///< G1: a byte without GPU data, CPU data to upload, no image there
        NotEveryFrame, ///< G2
        Count,
    };
    enum class Kind : u8 { Buffer, Image };
    struct Range {
        DAddr addr;
        u64 size;
        Kind kind;
        bool operator==(const Range&) const = default;
    };

    /// Frames a range must have been written in a row (VideoCommon::REDRAWN_WRITE_FRAMES).
    static constexpr u64 StreakFrames = 8;

    /// FRITCHII_ASYNC_COMPUTE_SKIP: 0 off, 1 skip, 2 dry (log, still wait). Any thread.
    [[nodiscard]] static int Mode() {
        const int value = VideoCore::Nx2Debug::SwitchValue(
            VideoCore::Nx2Debug::g_async_compute_skip, "FRITCHII_ASYNC_COMPUTE_SKIP", 0);
        return value == 1 || value == 2 ? value : 0;
    }
    /// FRITCHII_ASYNC_COMPUTE_SKIP_DELAY_MS (boot, test only): each compute program counts as
    /// unbuilt for this long after its first dispatch, a cold compile on a warm PC.
    [[nodiscard]] static int DelayMs() {
        static const int value = [] {
            const char* env = std::getenv("FRITCHII_ASYNC_COMPUTE_SKIP_DELAY_MS");
            const int ms = env != nullptr ? (std::max)(std::atoi(env), 0) : 0;
            if (ms > 0) {
                LOG_WARNING(Render_Vulkan,
                            "FRITCHII_ASYNC_COMPUTE_SKIP_DELAY_MS={}: every compute program "
                            "counts as unbuilt for {} ms after its first dispatch (test only)",
                            ms, ms);
            }
            return ms;
        }();
        return value;
    }

    ~AsyncComputeSkip() {
        // Joined first (a build in progress ends): the pipelines outlive this object.
        kick_workers.reset();
    }

    // --- The frame clock: the render thread's frames (TickFrame) ---

    /// R (or the GPU thread), at each presented frame: the clock, the 10 s line, pruning.
    void TickFrame() {
        const u64 now_frame = frame.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((now_frame & 63) == 0) {
            std::scoped_lock lock{mutex};
            if (streaks.size() > 16384) {
                std::erase_if(streaks, [now_frame](const auto& entry) {
                    return entry.second.last + StreakFrames < now_frame;
                });
            }
        }
        Report();
    }
    [[nodiscard]] u64 Frame() const noexcept {
        return frame.load(std::memory_order_relaxed);
    }

    // --- Is the pipeline unbuilt (E or R) ---

    [[nodiscard]] bool CountsUnbuilt(const ComputePipeline& pipeline) {
        if (!pipeline.IsBuilt()) {
            return true;
        }
        const int delay = DelayMs();
        if (delay <= 0) {
            return false;
        }
        std::scoped_lock lock{mutex};
        const auto now = Clock::now();
        const auto it = first_dispatch.try_emplace(pipeline.ShaderHash(), now).first;
        return now - it->second < std::chrono::milliseconds{delay};
    }

    // --- Write streaks (G2), the buffer cache's lock held by the caller ---

    /// The plain path, after a dispatch configured: its written ranges, stamped now.
    void NoteWrites(std::span<const Range> ranges) {
        if (ranges.empty()) {
            return;
        }
        std::scoped_lock lock{mutex};
        StampLocked(ranges, Frame());
    }
    /// E, a posted dispatch (ring B packet `number`): its ranges, stamped when R plays it, in R's
    /// frame (E runs ahead of R's frame clock).
    void QueueWrites(u64 number, std::span<const Range> ranges) {
        std::scoped_lock lock{mutex};
        if (queued.size() >= 4096) {
            queued.pop_front(); // a stop dropped plays: those writes count as missing (waits)
        }
        queued.push_back({number, {ranges.begin(), ranges.end()}});
        any_queued.store(true, std::memory_order_relaxed);
    }
    /// R, playing posted packet `number`.
    void PlayedWrites(u64 number) {
        if (!any_queued.load(std::memory_order_relaxed)) {
            return;
        }
        std::scoped_lock lock{mutex};
        while (!queued.empty() && queued.front().number <= number) {
            if (queued.front().number == number) {
                StampLocked(queued.front().ranges, Frame());
            }
            queued.pop_front();
        }
        any_queued.store(!queued.empty(), std::memory_order_relaxed);
    }

    // --- The decision's tests (R or the GPU thread) ---

    /// G2 for a buffer range: written in each of the last 8 frames (to this frame or the one
    /// before), or the chain.
    [[nodiscard]] bool BufferStreakHolds(const ComputePipeline& pipeline, const Range& range) {
        std::scoped_lock lock{mutex};
        const u64 now_frame = Frame();
        if (const auto it = streaks.find(Key{range.addr, range.size}); it != streaks.end()) {
            const Streak& streak = it->second;
            if ((streak.last == now_frame || streak.last + 1 == now_frame) &&
                streak.count >= StreakFrames) {
                return true;
            }
        }
        return ChainLocked(pipeline, range, now_frame);
    }
    /// G2's chain alone (images: the streak itself is the texture cache's).
    [[nodiscard]] bool ChainHolds(const ComputePipeline& pipeline, const Range& range) {
        std::scoped_lock lock{mutex};
        return ChainLocked(pipeline, range, Frame());
    }
    /// G3's sticky half: a guest read seen on a page of the range.
    [[nodiscard]] bool GuestReadSeen(const Range& range) {
        std::scoped_lock lock{mutex};
        const DAddr begin = range.addr & ~(PageSize - 1);
        const DAddr end = (range.addr + range.size + PageSize - 1) & ~(PageSize - 1);
        return read_pages.Intersects(begin, end - begin);
    }
    void NoteRefusal(Refusal why) {
        std::scoped_lock lock{mutex};
        ++window.refused[static_cast<size_t>(why)];
    }

    /// A dispatch skipped (mode 1) or that would be (mode 2) with these written ranges.
    void NoteSkip(const ComputePipeline& pipeline, std::span<const Range> ranges, bool dry) {
        std::scoped_lock lock{mutex};
        const auto now = Clock::now();
        const u64 now_frame = Frame();
        auto [it, fresh] = sessions.try_emplace(&pipeline);
        Session& session = it->second;
        if (!fresh && (session.hash != pipeline.ShaderHash() || session.dry != dry)) {
            // A retired pipeline's address taken by another, or the mode changed: a new session.
            session = Session{};
            fresh = true;
        }
        if (fresh) {
            session.hash = pipeline.ShaderHash();
            session.first = now;
            session.first_frame = now_frame;
            session.dry = dry;
            ++window.programs;
            if (logged_programs < 64) {
                ++logged_programs;
                LOG_INFO(Render_Vulkan,
                         "Async compute skip: pipeline {:016x} is still building, its dispatches "
                         "{} (outputs keep the last frame's GPU data; FRITCHII_ASYNC_COMPUTE_SKIP={})",
                         session.hash, dry ? "would be skipped (dry: they wait)" : "are skipped",
                         dry ? 2 : 1);
            }
            any_sessions.store(true, std::memory_order_relaxed);
        }
        session.last = now;
        session.last_frame = now_frame;
        ++session.count;
        for (const Range& range : ranges) {
            if (std::ranges::find(session.ranges, range) == session.ranges.end()) {
                session.ranges.push_back(range);
            }
        }
        ++(dry ? window.would_skip : window.skipped);
        if (!session.stuck && now - session.first > std::chrono::seconds{10}) {
            // Must be 0: the build should have ended long before.
            session.stuck = true;
            ++total.stuck;
            LOG_WARNING(Render_Vulkan,
                        "Async compute skip: pipeline {:016x} still skipped {:.1f} s after its "
                        "first skip ({} dispatches; must be 0)",
                        session.hash,
                        std::chrono::duration<double>(now - session.first).count(),
                        session.count);
        }
    }

    /// A dispatch of `pipeline` runs (built): ends its session. E or R.
    void NoteRan(const ComputePipeline& pipeline) {
        if (!any_sessions.load(std::memory_order_relaxed)) {
            return;
        }
        std::scoped_lock lock{mutex};
        const auto it = sessions.find(&pipeline);
        if (it == sessions.end()) {
            return;
        }
        if (it->second.hash != pipeline.ShaderHash()) {
            sessions.erase(it); // a retired pipeline's address, taken by another
            any_sessions.store(!sessions.empty(), std::memory_order_relaxed);
            return;
        }
        const Session& session = it->second;
        // The wait the first skip avoided: from it to the build's end (with the test delay, the
        // first dispatch plus the delay).
        Clock::time_point built = pipeline.BuiltAt();
        if (const int delay = DelayMs(); delay > 0) {
            if (const auto first = first_dispatch.find(session.hash);
                first != first_dispatch.end()) {
                built = (std::max)(built, first->second + std::chrono::milliseconds{delay});
            }
        }
        const double avoided_ms =
            built > session.first
                ? std::chrono::duration<double, std::milli>(built - session.first).count()
                : 0.0;
        window.avoided_ms += avoided_ms;
        if (logged_ends < 64) {
            ++logged_ends;
            LOG_INFO(Render_Vulkan,
                     "Async compute skip: pipeline {:016x} {} {} dispatches over {:.0f} ms (frames "
                     "{}-{}), ran at frame {}; {:.0f} ms of wait {}",
                     session.hash, session.dry ? "would have skipped" : "skipped", session.count,
                     std::chrono::duration<double, std::milli>(session.last - session.first)
                         .count(),
                     session.first_frame, session.last_frame, Frame(), avoided_ms,
                     session.dry ? "it would avoid" : "avoided");
        }
        sessions.erase(it);
        any_sessions.store(!sessions.empty(), std::memory_order_relaxed);
    }

    /// Any thread (GetFlushArea): a guest read of [addr, addr + size) on GPU-written or image
    /// pages. Sticky for G3; must be 0: a read of output whose last writer was skipped.
    void NoteGuestRead(DAddr addr, u64 size) {
        if (size == 0) {
            return;
        }
        const DAddr begin = addr & ~(PageSize - 1);
        const DAddr end = (addr + size + PageSize - 1) & ~(PageSize - 1);
        std::scoped_lock lock{mutex};
        read_pages.Add(begin, end - begin);
        for (const auto& [pipeline, session] : sessions) {
            if (session.dry) {
                continue; // it ran (waited): its output is there
            }
            for (const Range& range : session.ranges) {
                if (range.addr < addr + size && addr < range.addr + range.size) {
                    ++total.skipped_reads;
                    if (total.skipped_reads <= 8) {
                        LOG_WARNING(Render_Vulkan,
                                    "Async compute skip: the guest read {:#x}+{:#x}, written by "
                                    "the skipped pipeline {:016x} (must be 0)",
                                    addr, size, session.hash);
                    }
                    return;
                }
            }
        }
    }

    /// The first skip: the build starts on a spare thread now (the workers are first in, first
    /// out). R or the GPU thread.
    void Kick(ComputePipeline& pipeline) {
        if (pipeline.IsBuilt()) {
            return;
        }
        if (!kick_workers) {
            kick_workers = std::make_unique<Common::ThreadWorker>(2, "VkComputeKick");
        }
        pipeline.KickBuild(*kick_workers);
    }

private:
    static constexpr DAddr PageSize = 4096;

    struct Key {
        DAddr addr;
        u64 size;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        size_t operator()(const Key& key) const noexcept {
            return static_cast<size_t>(key.addr * 0x9E3779B97F4A7C15ULL ^ (key.size + (key.addr >> 17)));
        }
    };
    struct Streak {
        u64 last{};
        u64 count{};
    };
    struct Session {
        u64 hash{};
        Clock::time_point first{};
        Clock::time_point last{};
        u64 first_frame{};
        u64 last_frame{};
        u64 count{};
        bool dry{};
        bool stuck{};
        /// Written ranges skipped (the chain, and the must-be-0 read check)
        boost::container::small_vector<Range, 4> ranges;
    };
    struct Queued {
        u64 number{};
        boost::container::small_vector<Range, 4> ranges;
    };

    void StampLocked(std::span<const Range> ranges, u64 now_frame) {
        for (const Range& range : ranges) {
            if (range.kind != Kind::Buffer) {
                continue; // images keep their own streak (write_streak)
            }
            Streak& streak = streaks[Key{range.addr, range.size}];
            if (streak.count != 0 && streak.last == now_frame) {
                continue; // several writes in one frame count once
            }
            streak.count = streak.count != 0 && streak.last + 1 == now_frame ? streak.count + 1 : 1;
            streak.last = now_frame;
        }
    }

    [[nodiscard]] bool ChainLocked(const ComputePipeline& pipeline, const Range& range,
                                   u64 now_frame) const {
        const auto it = sessions.find(&pipeline);
        if (it == sessions.end() || it->second.dry || it->second.hash != pipeline.ShaderHash() ||
            it->second.last_frame + 1 < now_frame) {
            return false;
        }
        return std::ranges::find(it->second.ranges, range) != it->second.ranges.end();
    }

    void Report() {
        const auto now = Clock::now();
        if (report_at == Clock::time_point{}) {
            report_at = now;
            return;
        }
        if (now - report_at < std::chrono::seconds{10}) {
            return;
        }
        const double seconds = std::chrono::duration<double>(now - report_at).count();
        report_at = now;
        std::scoped_lock lock{mutex};
        // Sessions of pipelines not dispatched for 30 s (left the area, or retired): dropped.
        std::erase_if(sessions, [&](const auto& entry) {
            return now - entry.second.last > std::chrono::seconds{30};
        });
        any_sessions.store(!sessions.empty(), std::memory_order_relaxed);
        Window& w = window;
        u64 refused = 0;
        for (const u64 count : w.refused) {
            refused += count;
        }
        if (w.skipped + w.would_skip + refused != 0 || total.stuck + total.skipped_reads != 0) {
            LOG_INFO(Render_Vulkan,
                     "Async compute skip (FRITCHII_ASYNC_COMPUTE_SKIP={}) in the last {:.0f} s: "
                     "{:.1f} dispatches/s skipped, {:.1f}/s would skip (dry), {} new programs, "
                     "{:.0f} ms of waits avoided; unbuilt dispatches that waited: {} shape, {} "
                     "unknown extent, {} read back, {} no GPU data, {} not every frame; must be "
                     "0: {} programs still skipping 10 s after their first skip, {} guest reads "
                     "of skipped output",
                     Mode(), seconds, static_cast<double>(w.skipped) / seconds,
                     static_cast<double>(w.would_skip) / seconds, w.programs, w.avoided_ms,
                     w.refused[0], w.refused[1], w.refused[2], w.refused[3], w.refused[4],
                     total.stuck, total.skipped_reads);
        }
        w = Window{};
    }

    struct Window {
        u64 skipped{};
        u64 would_skip{};
        u64 programs{};
        double avoided_ms{};
        std::array<u64, static_cast<size_t>(Refusal::Count)> refused{};
    };
    struct Totals {
        u64 stuck{};
        u64 skipped_reads{};
    };

    std::mutex mutex;
    std::atomic<u64> frame{1};
    std::atomic_bool any_sessions{};
    std::atomic_bool any_queued{};
    std::unordered_map<Key, Streak, KeyHash> streaks;
    std::deque<Queued> queued;
    std::unordered_map<const ComputePipeline*, Session> sessions;
    std::unordered_map<u64, Clock::time_point> first_dispatch; ///< DELAY_MS only, by program
    Common::RangeSet<DAddr> read_pages;
    Window window;
    Totals total;
    u32 logged_programs{};
    u32 logged_ends{};
    Clock::time_point report_at{}; ///< TickFrame's thread only
    /// Last: joined first.
    std::unique_ptr<Common::ThreadWorker> kick_workers;
};

} // namespace Vulkan
