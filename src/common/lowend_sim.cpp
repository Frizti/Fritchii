// SPDX-FileCopyrightText: Copyright 2026 Fritchii
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/lowend_sim.h"

#ifdef _WIN32
#include <intrin.h>
#include <windows.h>
#endif

namespace Common::LowEndSim {

namespace {

u64 g_cpu_mask = 0;
std::vector<std::string> g_boot_notes;

[[nodiscard]] const char* SwitchText(const char* name) {
    const char* const value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && value[0] != '0' ? value : nullptr;
}

#ifdef _WIN32

/// The physical cores of processor group 0 as masks of their logical processors, lowest first.
std::vector<u64> CoreMasks() {
    std::vector<u64> cores;
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    std::vector<u8> buffer(length);
    auto* const first = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
    if (length == 0 || !GetLogicalProcessorInformationEx(RelationProcessorCore, first, &length)) {
        return cores;
    }
    for (DWORD offset = 0; offset < length;) {
        const auto* const entry =
            reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
        if (entry->Size == 0) {
            break;
        }
        if (entry->Processor.GroupCount > 0 && entry->Processor.GroupMask[0].Group == 0) {
            cores.push_back(static_cast<u64>(entry->Processor.GroupMask[0].Mask));
        }
        offset += entry->Size;
    }
    std::ranges::sort(cores);
    return cores;
}

/// FRITCHII_SIM_CPU=<cores>/<threads> (6/12, 4/8, 6/6; "6" = with SMT where the CPU has it). The
/// last cores: core 0 takes most of Windows' interrupts, and what is left stays free for the rest
/// of the PC. At least one core always stays out, so a test can't take the whole machine.
void ApplyCpuSet() {
    const char* const text = SwitchText("FRITCHII_SIM_CPU");
    if (text == nullptr) {
        return;
    }
    char* end = nullptr;
    const long cores = std::strtol(text, &end, 10);
    long threads = end != nullptr && *end == '/' ? std::strtol(end + 1, nullptr, 10) : 0;
    const std::vector<u64> all = CoreMasks();
    if (cores <= 0 || all.empty() || static_cast<size_t>(cores) >= all.size()) {
        g_boot_notes.push_back(fmt::format("FRITCHII_SIM_CPU={} refused: this PC has {} cores and "
                                           "at least one stays free",
                                           text, all.size()));
        return;
    }
    const bool smt = std::popcount(all.back()) > 1;
    if (threads <= 0) {
        threads = smt ? cores * 2 : cores;
    }
    threads = std::clamp(threads, cores, smt ? cores * 2 : cores);
    long extra = threads - cores;
    u64 mask = 0;
    for (long index = 0; index < cores; ++index) {
        u64 core = all[all.size() - 1 - static_cast<size_t>(index)];
        const u64 first = core & (~core + 1);
        mask |= first;
        core &= ~first;
        if (extra > 0 && core != 0) {
            mask |= core & (~core + 1);
            --extra;
        }
    }
    if (!SetProcessAffinityMask(GetCurrentProcess(), static_cast<DWORD_PTR>(mask))) {
        g_boot_notes.push_back(fmt::format("FRITCHII_SIM_CPU={}: SetProcessAffinityMask failed "
                                           "(error {})",
                                           text, GetLastError()));
        return;
    }
    g_cpu_mask = mask;
    g_boot_notes.push_back(fmt::format("FRITCHII_SIM_CPU={}: {} cores, {} threads, processors "
                                       "{:#x} of this PC's {} cores",
                                       text, cores, threads, mask, all.size()));
}

/// FRITCHII_SIM_RAM_LIMIT_MB=<MiB>: a job object holding this process alone. It closes with the
/// process. No kill-on-close: the limit never ends the game by itself. A job's CPU rate cap is not
/// offered: measured here it stops every thread for about 300 ms at a time, nothing like a slower
/// CPU (FRITCHII_SIM_CPU_SLOW is the speed knob).
void ApplyJob() {
    const char* const ram_text = SwitchText("FRITCHII_SIM_RAM_LIMIT_MB");
    if (ram_text == nullptr) {
        return;
    }
    const u64 ram_mib = std::strtoull(ram_text, nullptr, 10);
    // Under 4 GiB the game can't even boot: that would test nothing.
    if (ram_mib < 4096) {
        g_boot_notes.push_back(
            fmt::format("FRITCHII_SIM_RAM_LIMIT_MB={} ignored (at least 4096)", ram_text));
        return;
    }
    // Never closed: the job has to outlive every thread, and it ends with the process.
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        g_boot_notes.push_back(fmt::format("No job object (error {})", GetLastError()));
        return;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_JOB_MEMORY;
    info.JobMemoryLimit = static_cast<SIZE_T>(ram_mib << 20);
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info)) ||
        !AssignProcessToJobObject(job, GetCurrentProcess())) {
        g_boot_notes.push_back(
            fmt::format("FRITCHII_SIM_RAM_LIMIT_MB: not set (error {})", GetLastError()));
        CloseHandle(job);
        return;
    }
    g_boot_notes.push_back(fmt::format("Committed memory limit {} MiB", ram_mib));
}

struct ThreadState {
    u64 cycles{}; ///< the thread's own cycle counter at the last charge
    u64 tsc{};    ///< the clock at the last charge
    u64 debt{};   ///< cycles still to spin
    bool started{};
};
thread_local ThreadState t_state;

/// The charge's cheap gate, in clock ticks (about 10 us): reading a thread's cycle counter is a
/// system call, and the guest cores come through here at every return from the JIT.
constexpr u64 ChargeEveryTicks = 40'000;
/// Smaller debts wait for the next charge (about 20 us).
constexpr u64 MinSpinCycles = 80'000;
/// The longest single spin (about 5 ms); what is above it is forgotten, not paid later in a lump.
constexpr u64 MaxSpinCycles = 20'000'000;
/// A log line about this often (about 10 s).
constexpr u64 ReportEveryTicks = 40'000'000'000;

std::atomic<u64> g_worked_cycles{0};
std::atomic<u64> g_spun_cycles{0};
std::atomic<u64> g_next_report{0};

[[nodiscard]] u64 ThreadCycles() {
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    return cycles;
}

void ReportCpuSlow(u64 now_ticks, int percent) {
    u64 next = g_next_report.load(std::memory_order_relaxed);
    if (now_ticks < next ||
        !g_next_report.compare_exchange_strong(next, now_ticks + ReportEveryTicks,
                                               std::memory_order_relaxed)) {
        return;
    }
    const u64 worked = g_worked_cycles.exchange(0, std::memory_order_relaxed);
    const u64 spun = g_spun_cycles.exchange(0, std::memory_order_relaxed);
    if (next != 0 && worked != 0) {
        LOG_INFO(Common,
                 "Low-end simulation, CPU x{:.2f}: the slowed threads worked {:.1f} G cycles and "
                 "spun {:.1f} G more, x{:.2f} in all",
                 percent / 100.0, static_cast<double>(worked) / 1e9,
                 static_cast<double>(spun) / 1e9,
                 static_cast<double>(worked + spun) / static_cast<double>(worked));
    }
}

#endif // _WIN32

} // Anonymous namespace

void ApplyProcessLimits() {
#ifdef _WIN32
    ApplyCpuSet();
    ApplyJob();
#endif
}

void LogProcessLimits() {
    for (const std::string& note : g_boot_notes) {
        LOG_INFO(Common, "Low-end simulation: {}", note);
    }
}

u64 CpuMask() {
    return g_cpu_mask;
}

unsigned LogicalProcessors() {
    return g_cpu_mask != 0 ? static_cast<unsigned>(std::popcount(g_cpu_mask))
                           : std::thread::hardware_concurrency();
}

int FactorPercent(std::atomic<int>& value, const char* name, int max_percent) {
    int raw = value.load(std::memory_order_relaxed);
    if (raw < 0) {
        int parsed = 0;
        if (const char* const text = std::getenv(name); text != nullptr && text[0] != '\0') {
            const double number = std::strtod(text, nullptr);
            if (number >= 100.0) {
                parsed = static_cast<int>((std::min)(number, 100000.0));
            } else if (number >= 1.0) {
                parsed = static_cast<int>(std::lround(number * 100.0));
            }
        }
        int unset = raw;
        value.compare_exchange_strong(unset, parsed, std::memory_order_relaxed);
        raw = value.load(std::memory_order_relaxed);
    }
    if (raw <= 1) {
        return 100;
    }
    return (std::min)(raw < 100 ? raw * 100 : raw, max_percent);
}

void CpuCharge() {
#ifdef _WIN32
    ThreadState& state = t_state;
    const u64 ticks = __rdtsc();
    if (state.started && ticks - state.tsc < ChargeEveryTicks) {
        return; // the time since the last charge stays on the bill
    }
    state.tsc = ticks;
    const u64 now = ThreadCycles();
    if (!state.started) {
        state.started = true;
        state.cycles = now;
        return;
    }
    const u64 used = now - state.cycles;
    state.cycles = now;
    const int percent = CpuSlowPercent();
    if (percent <= 100) {
        state.debt = 0;
        return;
    }
    g_worked_cycles.fetch_add(used, std::memory_order_relaxed);
    state.debt += used / 100 * static_cast<u64>(percent - 100);
    if (state.debt >= MinSpinCycles) {
        const u64 spin = (std::min)(state.debt, MaxSpinCycles);
        state.debt = (std::min)(state.debt - spin, MaxSpinCycles);
        while (ThreadCycles() - now < spin) {
            _mm_pause();
        }
        g_spun_cycles.fetch_add(spin, std::memory_order_relaxed);
        state.cycles = ThreadCycles(); // the spin is not work to charge again
        state.tsc = __rdtsc();
    }
    ReportCpuSlow(state.tsc, percent);
#endif
}

void CpuRestart() {
#ifdef _WIN32
    ThreadState& state = t_state;
    state.cycles = ThreadCycles();
    state.tsc = __rdtsc();
    state.started = true;
#endif
}

} // namespace Common::LowEndSim
