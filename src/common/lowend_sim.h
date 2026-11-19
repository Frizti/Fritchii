// SPDX-FileCopyrightText: Copyright 2026 Fritchii
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

/// Test switches that make a fast PC behave more like a weak one, so performance work can be
/// judged for low-end hardware (docs/fritchii/switches.md, "Low-end PC simulation";
/// work\lowend_sim.py sets them from presets). Everything is off by default and applies to this
/// process only: no setting of the PC changes, and it all ends when the process exits.
namespace Common::LowEndSim {

/// main(), once nx2_switches.txt is in the environment and before any worker thread starts:
/// FRITCHII_SIM_CPU (the processors this process may use) and FRITCHII_SIM_RAM_LIMIT_MB (a job
/// object holding only this process).
void ApplyProcessLimits();

/// What ApplyProcessLimits did, for the log (logging starts after it).
void LogProcessLimits();

/// FRITCHII_SIM_CPU's logical processors of group 0; 0 = no limit.
[[nodiscard]] u64 CpuMask();

/// std::thread::hardware_concurrency(), or FRITCHII_SIM_CPU's thread count: the number the thread
/// pools size themselves by. An affinity mask alone does not change hardware_concurrency.
[[nodiscard]] unsigned LogicalProcessors();

/// A slowdown factor in hundredths (350 = 3.5x), from its switch. Environment: "3.5" or "350".
/// Live (whole numbers only): under 100 is a whole factor (3 = 3x), else hundredths. 0 or 1: off.
/// At most `max_percent`.
[[nodiscard]] int FactorPercent(std::atomic<int>& value, const char* name, int max_percent);

/// FRITCHII_SIM_CPU_SLOW (live): each hooked thread's work takes `factor` times as long.
inline std::atomic<int> g_cpu_slow{-1};
/// FRITCHII_SIM_GPU_SLOW (live): the GPU is kept busy for (factor - 1) x its measured work.
inline std::atomic<int> g_gpu_slow{-1};

inline constexpr int MaxCpuSlowPercent = 400;
inline constexpr int MaxGpuSlowPercent = 1000;

[[nodiscard]] inline int CpuSlowPercent() {
    return FactorPercent(g_cpu_slow, "FRITCHII_SIM_CPU_SLOW", MaxCpuSlowPercent);
}
[[nodiscard]] inline int GpuSlowPercent() {
    return FactorPercent(g_gpu_slow, "FRITCHII_SIM_GPU_SLOW", MaxGpuSlowPercent);
}

namespace Detail {
[[nodiscard]] inline bool FactorOn(std::atomic<int>& value, const char* name, int max_percent) {
    const int raw = value.load(std::memory_order_relaxed);
    if (raw < 0) [[unlikely]] {
        return FactorPercent(value, name, max_percent) > 100;
    }
    return raw > 1 && raw != 100;
}
} // namespace Detail

/// Off: one relaxed load and a compare.
[[nodiscard]] inline bool CpuSlowOn() {
    return Detail::FactorOn(g_cpu_slow, "FRITCHII_SIM_CPU_SLOW", MaxCpuSlowPercent);
}
[[nodiscard]] inline bool GpuSlowOn() {
    return Detail::FactorOn(g_gpu_slow, "FRITCHII_SIM_GPU_SLOW", MaxGpuSlowPercent);
}

/// The threads FRITCHII_SIM_CPU_SLOW slows, for its log line.
enum class CpuClass : u8 { Game, Front, Back, Render, Worker, Count };

void CpuWorkBeginSlow(CpuClass work_class);
void CpuWorkEndSlow();
void CpuWorkCancelSlow();
void CpuPauseNoteSlow();
[[nodiscard]] bool CpuWaitBeginSlow();
void CpuWaitEndSlow();

/// A stretch of real work on this thread starts. Only the time between Begin and End counts, once:
/// a Begin inside an open stretch does nothing. At End the thread spins for (factor - 1) x the work.
inline void CpuWorkBegin(CpuClass work_class) {
    if (CpuSlowOn()) [[unlikely]] {
        CpuWorkBeginSlow(work_class);
    }
}
inline void CpuWorkEnd() {
    if (CpuSlowOn()) [[unlikely]] {
        CpuWorkEndSlow();
    }
}
/// The open stretch is dropped: the thread went idle (a guest core with nothing to run).
inline void CpuWorkCancel() {
    if (CpuSlowOn()) [[unlikely]] {
        CpuWorkCancelSlow();
    }
}
/// From a spin loop's pause. The gaps between pauses of a loop are not work, so waiting for another
/// thread is not charged, which would make every wait longer than the work it waited for.
inline void CpuPauseNote() {
    if (CpuSlowOn()) [[unlikely]] {
        CpuPauseNoteSlow();
    }
}

/// A wait for another thread or the GPU inside a stretch of work: left out of it.
class CpuWaitScope {
public:
    CpuWaitScope() : active{CpuSlowOn() && CpuWaitBeginSlow()} {}
    ~CpuWaitScope() {
        if (active) [[unlikely]] {
            CpuWaitEndSlow();
        }
    }
    CpuWaitScope(const CpuWaitScope&) = delete;
    CpuWaitScope& operator=(const CpuWaitScope&) = delete;

private:
    bool active;
};

} // namespace Common::LowEndSim
