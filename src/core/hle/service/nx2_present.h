// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The present bookkeeping of NVN direct mode's presents (core/nvn_hook.cpp PresentDirect), which
// skip nx2:vi2's PresentClientBuffer: counts and pacing. Fritchii keeps its nx2:vi2 presenter as
// it is (its OoT pacing and high refresh controls live there); direct mode's game still starts its
// frames on nx2:vi2's vsync event, so this only holds a present until its swap interval is up.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>

#include "common/settings.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/nx2_thread_stats.h"

namespace Service {

/// nx2:vi2's present pacing (INx2Vi2Session::PaceToVblank: Ocarina of Time's high refresh rate
/// and 30 FPS modes, its speed scale, the genlock), set by the session that owns the vsync event.
/// Direct mode's presents skip nx2:vi2, so they go through it here.
inline std::mutex g_vi2_pacer_mutex;
inline std::function<void(u32, Kernel::KProcess*)> g_vi2_pacer;
inline const void* g_vi2_pacer_owner{};

/// nx2:vi2's HDR change checks (Lichii): Fritchii's presenter labels no HDR, so there are none.
inline void Nx2NotifyHdrChange(Kernel::KernelCore&) {}

class Nx2PresentBookkeeping {
public:
    explicit Nx2PresentBookkeeping(Core::System& system_) : system{system_} {}

    /// Counts a present made on `thread`; its number among every present of the process.
    u64 Count(Kernel::KThread& thread) {
        if (Kernel::Nx2ThreadStats::g_enabled) {
            Kernel::Nx2ThreadStats::OnPresent(&thread);
        }
        return Core::g_nx2_present_count.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    /// Holds the present until its swap interval of vblanks at its refresh rate (word 12:
    /// (interval << 16) | rate) has passed since the previous one, at the speed limit.
    void PaceToVblank(u32 timing, const Kernel::KProcess* process, u64, const void*) {
        {
            std::scoped_lock lock{g_vi2_pacer_mutex};
            if (g_vi2_pacer) {
                g_vi2_pacer(timing, const_cast<Kernel::KProcess*>(process));
                return;
            }
        }
        if (!Settings::SpeedLimitEnabled() || Settings::SpeedLimit() == 0) {
            deadline = {};
            return;
        }
        const u32 interval = std::clamp<u32>(timing >> 16, 1, 4);
        const u32 rate = (timing & 0xffff) >= 30 && (timing & 0xffff) <= 240 ? timing & 0xffff : 60;
        const auto period = std::chrono::nanoseconds{100'000'000'000LL * interval / rate /
                                                     Settings::SpeedLimit()};
        const auto now = std::chrono::steady_clock::now();
        if (deadline == std::chrono::steady_clock::time_point{} || now > deadline + period * 2) {
            deadline = now + period;   // the first present, or far behind: no catching up
            return;
        }
        if (now < deadline) {
            std::this_thread::sleep_until(deadline);
        }
        deadline += period;
    }

    void Finish(u64) {}

private:
    Core::System& system;
    std::chrono::steady_clock::time_point deadline{};
};

} // namespace Service
