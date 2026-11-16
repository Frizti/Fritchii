// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace Common {

struct MemoryInfo {
    u64 TotalPhysicalMemory{};
    u64 TotalSwapMemory{};
};

/**
 * Gets the memory info of the host system
 * @return Reference to a MemoryInfo struct with the physical and swap memory sizes in bytes
 */
[[nodiscard]] const MemoryInfo& GetMemInfo();

/// Page faults of this process so far, soft and hard (0 where unknown).
[[nodiscard]] u64 GetProcessPageFaults();

/// This process's memory and the system's commit, for the memory log line (zeros where unknown;
/// only Windows fills them in).
/// Commit the system can still hand out, in bytes: its commit limit (RAM plus the page files)
/// minus what is committed. No value where the host reports no such limit (anything but Windows).
/// Ported.
[[nodiscard]] std::optional<u64> GetSystemCommitRoom();

struct ProcessMemoryUsage {
    u64 working_set{};
    u64 peak_working_set{};
    u64 private_bytes{}; ///< Private commit (mapped sections like the guest memory are not in it)
    u64 peak_private_bytes{};
    /// Resident pages not shareable with other mappings: the working set minus the guest memory
    /// section and mapped files (0 where Windows does not report it: older builds).
    u64 private_working_set{};
    u64 system_commit{}; ///< Commit charge of the whole system
    u64 system_commit_limit{};
    u64 available_physical{};
};
[[nodiscard]] ProcessMemoryUsage GetProcessMemoryUsage();

/// This process's Win32 heaps (the C runtime's malloc heap and every other HeapCreate heap: Qt's,
/// drivers' that use one), summed with HeapSummary. Zeros elsewhere.
struct HeapUsage {
    u64 crt_committed{};   ///< The C runtime's heap (malloc, new)
    u64 crt_allocated{};
    u64 other_committed{}; ///< Every other heap of the process
    u64 other_allocated{};
    u32 heaps{};           ///< Heaps summed, the C runtime's included
    u32 micros{};          ///< What the pass took
};
/// Locks each heap for its summary: call it off the busy threads. Also kept for LastHeapUsage.
HeapUsage SampleHeapUsage();
/// The last SampleHeapUsage result (zeros before the first).
[[nodiscard]] HeapUsage LastHeapUsage();

/// Committed memory of zeros that takes RAM only where it is touched (VirtualAlloc, anonymous
/// mmap); null on failure. For big tables mostly left zero.
[[nodiscard]] void* AllocateZeroPages(u64 bytes);
void FreeZeroPages(void* memory, u64 bytes);

/// FRITCHII_MEM_CENSUS (boot, default 0; docs/fritchii/switches.md): every 30 s, and whenever the
/// working set climbs 512 MiB past the last census (a load peak), walks this process's address
/// space (VirtualQuery and one working set query on Windows, /proc/self/smaps on Linux) and logs
/// committed and resident MiB by owner: tagged ranges (guest memory, VMA's host maps), images
/// (DLLs, drivers), heaps, stacks, executable private memory (the JIT code caches), and the rest
/// of the private memory by reservation size with its largest allocations. Read-only.
/// 1 = committed and resident, 2 = also every heap's summary (locks each heap: can hitch),
/// 3 = committed only (no working set query).
namespace MemCensus {
[[nodiscard]] int Mode();
/// A named range for the census (`name` must outlive it: a string literal). No-op when off.
void Tag(const char* name, const void* base, u64 size);
void Untag(const void* base);
/// Text the census adds to its log (guest residency, JIT compiles); called on the census thread.
void SetGuestSampler(std::function<std::string()> sampler);
/// Sizes of memory some library allocates where the census can't see its address (the driver's
/// copies of Vulkan device memory): private allocations of the same size (64 KiB granules) are
/// counted under `label` ("size match"). `label` must be a string literal. Called on the census
/// thread; null removes it.
void SetSizeSource(const char* label, std::function<std::vector<u64>()> source);
/// Starts the census thread when Mode() is not 0 (once per Start/Stop pair).
void Start();
/// Stops and joins it (before what the sampler reads goes away).
void Stop();
} // namespace MemCensus

} // namespace Common
