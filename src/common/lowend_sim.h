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

/// FRITCHII_SIM_CPU_SLOW (live): each hooked thread spins for (factor - 1) x the CPU time it used.
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

/// The calling thread pays for the CPU time it used since its last CpuCharge or CpuRestart: it
/// spins for (factor - 1) x that time, counted in its own CPU cycles, so being descheduled does
/// not count. Small amounts add up until they are worth a spin. Call only with CpuSlowOn().
void CpuCharge();

/// The calling thread's CPU time counts from here (after a wait that may have spun: waiting is
/// not work a slower CPU would take longer over).
void CpuRestart();

} // namespace Common::LowEndSim
