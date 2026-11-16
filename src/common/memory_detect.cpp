// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <sysinfoapi.h>
#include <psapi.h>
// clang-format on
#else
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/types.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#else
#include <unistd.h>
#endif
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#include <malloc.h>
#endif

#include <fmt/format.h>

#include "common/logging.h"
#include "common/memory_detect.h"
#include "common/nx2_hitch.h"
#include "common/thread.h"

namespace Common {

// Detects the RAM and Swapfile sizes
static MemoryInfo Detect() {
    MemoryInfo mem_info{};

#ifdef _WIN32
    MEMORYSTATUSEX memorystatus;
    memorystatus.dwLength = sizeof(memorystatus);
    GlobalMemoryStatusEx(&memorystatus);
    mem_info.TotalPhysicalMemory = memorystatus.ullTotalPhys;
    mem_info.TotalSwapMemory = memorystatus.ullTotalPageFile - mem_info.TotalPhysicalMemory;
#elif defined(__APPLE__)
    u64 ramsize;
    struct xsw_usage vmusage;
    std::size_t sizeof_ramsize = sizeof(ramsize);
    std::size_t sizeof_vmusage = sizeof(vmusage);
    // hw and vm are defined in sysctl.h
    // https://github.com/apple/darwin-xnu/blob/master/bsd/sys/sysctl.h#L471
    // sysctlbyname(const char *, void *, size_t *, void *, size_t);
    sysctlbyname("hw.memsize", &ramsize, &sizeof_ramsize, nullptr, 0);
    sysctlbyname("vm.swapusage", &vmusage, &sizeof_vmusage, nullptr, 0);
    mem_info.TotalPhysicalMemory = ramsize;
    mem_info.TotalSwapMemory = vmusage.xsu_total;
#elif defined(__FreeBSD__)
    u_long physmem, swap_total;
    std::size_t sizeof_u_long = sizeof(u_long);
    // sysctlbyname(const char *, void *, size_t *, const void *, size_t);
    sysctlbyname("hw.physmem", &physmem, &sizeof_u_long, nullptr, 0);
    sysctlbyname("vm.swap_total", &swap_total, &sizeof_u_long, nullptr, 0);
    mem_info.TotalPhysicalMemory = physmem;
    mem_info.TotalSwapMemory = swap_total;
#elif defined(__linux__)
    struct sysinfo meminfo;
    sysinfo(&meminfo);
    mem_info.TotalPhysicalMemory = meminfo.totalram;
    mem_info.TotalSwapMemory = meminfo.totalswap;
#else
    mem_info.TotalPhysicalMemory = sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGE_SIZE);
    mem_info.TotalSwapMemory = 0;
#endif

    return mem_info;
}

const MemoryInfo& GetMemInfo() {
    static MemoryInfo mem_info = Detect();
    return mem_info;
}

u64 GetProcessPageFaults() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return 0;
    }
    return counters.PageFaultCount;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
    return static_cast<u64>(usage.ru_minflt) + static_cast<u64>(usage.ru_majflt);
#endif
}

ProcessMemoryUsage GetProcessMemoryUsage() {
    ProcessMemoryUsage usage{};
#ifdef _WIN32
    // PROCESS_MEMORY_COUNTERS_EX2's layout (the SDK declares it only for NTDDI_WIN10_CU and up).
    // Windows builds that know it fill PrivateWorkingSetSize; older ones either refuse the size
    // (then the EX one is asked for) or leave it 0.
    struct CountersEx2 {
        PROCESS_MEMORY_COUNTERS_EX ex;
        SIZE_T PrivateWorkingSetSize;
        ULONG64 SharedCommitUsage;
    };
    CountersEx2 counters{};
    counters.ex.cb = sizeof(counters);
    bool ok = K32GetProcessMemoryInfo(GetCurrentProcess(),
                                      reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                      sizeof(counters));
    if (!ok) {
        counters = {};
        counters.ex.cb = sizeof(counters.ex);
        ok = K32GetProcessMemoryInfo(GetCurrentProcess(),
                                     reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters.ex),
                                     sizeof(counters.ex));
    }
    if (ok) {
        usage.working_set = counters.ex.WorkingSetSize;
        usage.peak_working_set = counters.ex.PeakWorkingSetSize;
        usage.private_bytes = counters.ex.PrivateUsage;
        usage.peak_private_bytes = counters.ex.PeakPagefileUsage;
        usage.private_working_set = counters.PrivateWorkingSetSize;
    }
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        usage.system_commit = status.ullTotalPageFile - status.ullAvailPageFile;
        usage.system_commit_limit = status.ullTotalPageFile;
        usage.available_physical = status.ullAvailPhys;
    }
#endif
    return usage;
}

namespace {
std::mutex g_heap_usage_mutex;
HeapUsage g_heap_usage{};
} // Anonymous namespace

HeapUsage SampleHeapUsage() {
    HeapUsage usage{};
#ifdef _WIN32
    const auto start = std::chrono::steady_clock::now();
#ifdef _MSC_VER
    const HANDLE crt_heap = reinterpret_cast<HANDLE>(_get_heap_handle());
#else
    const HANDLE crt_heap = GetProcessHeap();
#endif
    std::vector<HANDLE> heaps(64);
    for (int attempt = 0; attempt < 4; ++attempt) {
        const DWORD count = GetProcessHeaps(static_cast<DWORD>(heaps.size()), heaps.data());
        if (count <= heaps.size()) {
            heaps.resize(count);
            break;
        }
        heaps.resize(count + 16);
    }
    for (const HANDLE heap : heaps) {
        HEAP_SUMMARY summary{};
        summary.cb = sizeof(summary);
        if (!HeapSummary(heap, 0, &summary)) {
            continue;
        }
        ++usage.heaps;
        if (heap == crt_heap) {
            usage.crt_committed += summary.cbCommitted;
            usage.crt_allocated += summary.cbAllocated;
        } else {
            usage.other_committed += summary.cbCommitted;
            usage.other_allocated += summary.cbAllocated;
        }
    }
    usage.micros = static_cast<u32>(std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - start)
                                        .count());
#endif
    std::scoped_lock lock{g_heap_usage_mutex};
    g_heap_usage = usage;
    return usage;
}

HeapUsage LastHeapUsage() {
    std::scoped_lock lock{g_heap_usage_mutex};
    return g_heap_usage;
}

std::optional<u64> GetSystemCommitRoom() {
#ifdef _WIN32
    // GlobalMemoryStatusEx, not GetPerformanceInfo: the latter also counts every process, thread
    // and handle in the system and took ~3.7 ms a call (388 processes, 176k handles), on the GPU
    // back thread every 30 frames (TextureCache::TickFrame, ~8.7 ms/s at the Castle Town market).
    // GlobalMemoryStatusEx takes ~1.4 us and its ullAvailPageFile is the same number (commit limit
    // minus commit total; equal to the byte in a side-by-side read), only ever lower inside a job
    // object with a commit quota, where the quota is the tighter real bound anyway.
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        return status.ullAvailPageFile;
    }
#endif
    return std::nullopt;
}

void* AllocateZeroPages(u64 bytes) {
#ifdef _WIN32
    return VirtualAlloc(nullptr, static_cast<SIZE_T>(bytes), MEM_RESERVE | MEM_COMMIT,
                        PAGE_READWRITE);
#else
    void* const memory = mmap(nullptr, static_cast<size_t>(bytes), PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return memory == MAP_FAILED ? nullptr : memory;
#endif
}

void FreeZeroPages(void* memory, u64 bytes) {
    if (memory == nullptr) {
        return;
    }
#ifdef _WIN32
    static_cast<void>(bytes);
    VirtualFree(memory, 0, MEM_RELEASE);
#else
    munmap(memory, static_cast<size_t>(bytes));
#endif
}

// FRITCHII_MEM_CENSUS (memory_detect.h): who holds the working set. The "Memory owners" line left
// ~3.7 GB of the private working set unattributed at the Castle Town market (2026-11-16); this
// walks the address space instead of estimating.
namespace MemCensus {
namespace {

struct TagEntry {
    const char* name;
    uintptr_t base;
    u64 size;
};
std::mutex g_tag_mutex;
std::vector<TagEntry> g_tags;             ///< Sorted by base
std::function<std::string()> g_sampler;   ///< Under g_tag_mutex
std::vector<std::pair<const char*, std::function<std::vector<u64>()>>> g_size_sources; ///< Same
std::mutex g_thread_mutex;
std::jthread g_thread;

struct Amount {
    u64 committed{};
    u64 resident{};
    u32 count{};
    void Add(u64 c, u64 r) {
        committed += c;
        resident += r;
    }
};

/// One allocation (an AllocationBase on Windows, a mapping on Linux) after the walk.
struct Allocation {
    uintptr_t base{};
    u64 span{};       ///< Reserved bytes from the base to the end of its last region
    u64 committed{};  ///< Committed and not inside a tagged range
    u64 resident{};
    u32 protect{};    ///< The protection of its largest committed region (Windows)
    std::string name; ///< Image or mapped file
};

/// Everything a census found, the same on every host.
struct Result {
    std::map<std::string, Amount> tagged;
    std::map<std::string, Amount> matched; ///< SetSizeSource matches
    std::vector<Allocation> big_blocks;    ///< Heap blocks with an allocation of their own
    Amount crt_segments;                   ///< Further segments of the CRT heap
    Amount other_segments;                 ///< Further segments of the other heaps
    std::vector<Allocation> images;
    std::vector<Allocation> mapped;
    std::vector<Allocation> exec;
    std::vector<Allocation> other;
    Amount heaps;
    Amount crt_heap;
    Amount stacks;
    std::array<Amount, 6> buckets{}; ///< Other private by reservation size
    u64 reserved{};
    u64 regions{};
    u64 ws_pages{};
    u32 walk_ms{};
    u32 query_ms{};
    std::string heap_text;
};

constexpr std::array<const char*, 6> BucketNames{"<64 KiB",  "64 KiB-1 MiB", "1-16 MiB",
                                                 "16-64 MiB", "64-256 MiB",  "256 MiB+"};
size_t BucketOf(u64 span) {
    constexpr u64 KiB = 1024;
    constexpr u64 MiB = KiB * KiB;
    if (span < 64 * KiB) {
        return 0;
    }
    if (span < MiB) {
        return 1;
    }
    if (span < 16 * MiB) {
        return 2;
    }
    if (span < 64 * MiB) {
        return 3;
    }
    return span < 256 * MiB ? 4 : 5;
}

u64 MiBOf(u64 bytes) {
    return bytes >> 20;
}

std::vector<TagEntry> CopyTags() {
    std::scoped_lock lock{g_tag_mutex};
    return g_tags;
}

/// Splits [begin, end) into the tagged parts (added to result.tagged) and returns the untagged
/// bytes, with `resident_of` giving a range's resident bytes.
template <typename ResidentOf>
std::pair<u64, u64> SplitTagged(const std::vector<TagEntry>& tags, uintptr_t begin, uintptr_t end,
                                Result& result, ResidentOf&& resident_of) {
    u64 tagged_bytes = 0;
    u64 tagged_resident = 0;
    auto it = std::upper_bound(tags.begin(), tags.end(), begin,
                               [](uintptr_t value, const TagEntry& tag) { return value < tag.base; });
    if (it != tags.begin()) {
        --it;
    }
    for (; it != tags.end() && it->base < end; ++it) {
        const uintptr_t tag_end = it->base + it->size;
        if (tag_end <= begin) {
            continue;
        }
        const uintptr_t overlap_begin = (std::max)(begin, it->base);
        const uintptr_t overlap_end = (std::min)(end, tag_end);
        const u64 resident = resident_of(overlap_begin, overlap_end);
        result.tagged[it->name].Add(overlap_end - overlap_begin, resident);
        tagged_bytes += overlap_end - overlap_begin;
        tagged_resident += resident;
    }
    return {(end - begin) - tagged_bytes, resident_of(begin, end) - tagged_resident};
}

std::string ProtectText(u32 protect) {
#ifdef _WIN32
    std::string text;
    switch (protect & 0xff) {
    case PAGE_NOACCESS: text = "none"; break;
    case PAGE_READONLY: text = "r"; break;
    case PAGE_READWRITE: text = "rw"; break;
    case PAGE_WRITECOPY: text = "wc"; break;
    case PAGE_EXECUTE: text = "x"; break;
    case PAGE_EXECUTE_READ: text = "rx"; break;
    case PAGE_EXECUTE_READWRITE: text = "rwx"; break;
    case PAGE_EXECUTE_WRITECOPY: text = "xwc"; break;
    default: text = fmt::format("{:#x}", protect & 0xff); break;
    }
    if ((protect & PAGE_GUARD) != 0) {
        text += "+guard";
    }
    if ((protect & PAGE_NOCACHE) != 0) {
        text += "+nocache";
    }
    if ((protect & PAGE_WRITECOMBINE) != 0) {
        text += "+wc";
    }
    return text;
#else
    return fmt::format("{:#x}", protect);
#endif
}

/// SetSizeSource's sizes, in 64 KiB granules, taken one by one as allocations match them.
struct SizeMatcher {
    std::vector<std::pair<std::string, std::multiset<u64>>> sources;

    static u64 Granules(u64 bytes) {
        return (bytes + 0xffff) >> 16;
    }
    void Load() {
        std::vector<std::pair<const char*, std::function<std::vector<u64>()>>> copy;
        {
            std::scoped_lock lock{g_tag_mutex};
            copy = g_size_sources;
        }
        for (auto& [label, source] : copy) {
            std::multiset<u64> sizes;
            for (const u64 size : source()) {
                sizes.insert(Granules(size));
            }
            sources.emplace_back(label, std::move(sizes));
        }
    }
    /// The label of the source an allocation of `span` reserved bytes matches (taking that size).
    const std::string* Take(u64 span) {
        const u64 granules = Granules(span);
        for (auto& [label, sizes] : sources) {
            if (const auto it = sizes.find(granules); it != sizes.end()) {
                sizes.erase(it);
                return &label;
            }
        }
        return nullptr;
    }
};

#ifdef _WIN32
std::string Utf8Of(const wchar_t* text) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), length, nullptr, nullptr);
    return out;
}

std::string BaseName(std::string path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/// Sorted page addresses of the working set; empty when the query failed.
std::vector<uintptr_t> WorkingSetPages() {
    std::vector<ULONG_PTR> entries(size_t{1} << 22);
    for (int attempt = 0;; ++attempt) {
        if (K32QueryWorkingSet(GetCurrentProcess(), entries.data(),
                               static_cast<DWORD>(entries.size() * sizeof(ULONG_PTR)))) {
            break;
        }
        if (GetLastError() != ERROR_BAD_LENGTH || attempt == 3) {
            return {};
        }
        entries.assign((std::max)(entries.size() * 2, size_t{entries[0]} + (size_t{1} << 18)), 0);
    }
    const size_t count = (std::min)(size_t{entries[0]}, entries.size() - 1);
    std::vector<uintptr_t> pages(count);
    for (size_t i = 0; i < count; ++i) {
        pages[i] = entries[i + 1] & ~uintptr_t{0xfff};
    }
    std::sort(pages.begin(), pages.end());
    return pages;
}

/// FRITCHII_MEM_CENSUS=4, once: the CRT heap's busy blocks by size, to tell owners apart by their
/// block sizes (a hash map's nodes, a JIT table's entries, pipeline objects). HeapWalk under
/// HeapLock: every thread that allocates waits for the whole walk (~0.3-1 s for ~3 GB). Nothing is
/// allocated from the heap while it is locked: the tables are made before.
std::string CrtHeapHistogram(HANDLE heap) {
    constexpr size_t Slots = 8192; // exact sizes, open addressing
    struct Slot {
        u64 size{};
        u64 count{};
    };
    std::vector<Slot> slots(Slots);
    std::array<u64, 40> bucket_count{};
    std::array<u64, 40> bucket_bytes{};
    u64 blocks = 0;
    u64 bytes = 0;
    u64 overflow = 0;
    const auto start = std::chrono::steady_clock::now();
    if (!HeapLock(heap)) {
        return "CRT heap walk: HeapLock failed";
    }
    PROCESS_HEAP_ENTRY entry{};
    while (HeapWalk(heap, &entry)) {
        if ((entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) == 0) {
            continue;
        }
        const u64 size = entry.cbData;
        ++blocks;
        bytes += size;
        const size_t bucket = (std::min<size_t>)(size == 0 ? 0 : 64 - std::countl_zero(size), 39);
        ++bucket_count[bucket];
        bucket_bytes[bucket] += size;
        size_t index = static_cast<size_t>((size * 0x9e3779b97f4a7c15ULL) >> 51) % Slots;
        size_t probes = 0;
        while (slots[index].count != 0 && slots[index].size != size && probes < 64) {
            index = (index + 1) % Slots;
            ++probes;
        }
        if (slots[index].count == 0 || slots[index].size == size) {
            slots[index].size = size;
            ++slots[index].count;
        } else {
            ++overflow;
        }
    }
    HeapUnlock(heap);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    std::string text = fmt::format("CRT heap walk ({} ms): {} busy blocks, {} MiB; by size "
                                   "(blocks/MiB):",
                                   ms, blocks, MiBOf(bytes));
    for (size_t i = 0; i < bucket_count.size(); ++i) {
        if (bucket_count[i] != 0) {
            text += fmt::format(" <{} {}/{}", u64{1} << i, bucket_count[i], MiBOf(bucket_bytes[i]));
        }
    }
    std::sort(slots.begin(), slots.end(), [](const Slot& a, const Slot& b) {
        return a.size * a.count > b.size * b.count;
    });
    text += "; top exact sizes (bytes x blocks = MiB):";
    for (size_t i = 0; i < 16 && slots[i].count != 0; ++i) {
        text += fmt::format(" {}x{}={}", slots[i].size, slots[i].count,
                            MiBOf(slots[i].size * slots[i].count));
    }
    if (overflow != 0) {
        text += fmt::format(" ({} blocks of sizes not tabled)", overflow);
    }
    return text;
}

void Walk(int mode, Result& result) {
    const auto start = std::chrono::steady_clock::now();
    std::vector<uintptr_t> pages;
    if (mode != 3) {
        const Nx2Hitch::Scope timed{Nx2Hitch::Cause::MemorySample};
        pages = WorkingSetPages();
    }
    result.ws_pages = pages.size();
    // The query's own buffer is gone again; the pages vector is the census's own.
    std::vector<TagEntry> tags = CopyTags();
    if (!pages.empty()) {
        tags.push_back({"census scratch", reinterpret_cast<uintptr_t>(pages.data()),
                        pages.capacity() * sizeof(uintptr_t)});
        std::sort(tags.begin(), tags.end(),
                  [](const TagEntry& a, const TagEntry& b) { return a.base < b.base; });
    }
    result.query_ms = static_cast<u32>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - start)
                                           .count());
    const auto resident_of = [&pages](uintptr_t begin, uintptr_t end) -> u64 {
        const auto low = std::lower_bound(pages.begin(), pages.end(), begin);
        const auto high = std::lower_bound(low, pages.end(), end);
        return static_cast<u64>(high - low) * 4096;
    };

#ifdef _MSC_VER
    const auto crt_heap = static_cast<uintptr_t>(_get_heap_handle());
#else
    const auto crt_heap = reinterpret_cast<uintptr_t>(GetProcessHeap());
#endif
    std::vector<HANDLE> heap_handles(256);
    const DWORD heap_count =
        GetProcessHeaps(static_cast<DWORD>(heap_handles.size()), heap_handles.data());
    heap_handles.resize((std::min)(static_cast<size_t>(heap_count), heap_handles.size()));
    std::vector<uintptr_t> heaps;
    for (const HANDLE heap : heap_handles) {
        heaps.push_back(reinterpret_cast<uintptr_t>(heap));
    }
    std::sort(heaps.begin(), heaps.end());

    SizeMatcher matcher;
    matcher.Load();

    struct Current {
        Allocation allocation;
        DWORD type{};
        bool exec{};
        bool guard{};
        bool first_rw{}; ///< Its first page is committed read/write (a heap header can be read)
        u64 largest_region{};
    };
    Current current{};
    bool have_current = false;
    const auto flush = [&] {
        if (!have_current) {
            return;
        }
        have_current = false;
        Allocation& a = current.allocation;
        if (a.committed == 0) {
            return;
        }
        if (current.type == MEM_IMAGE) {
            wchar_t path[MAX_PATH]{};
            if (GetModuleFileNameW(reinterpret_cast<HMODULE>(a.base), path, MAX_PATH) != 0) {
                a.name = BaseName(Utf8Of(path));
            }
            if (a.name.empty()) {
                a.name = fmt::format("image@{:x}", a.base);
            }
            result.images.push_back(std::move(a));
            return;
        }
        if (current.type == MEM_MAPPED) {
            wchar_t path[MAX_PATH]{};
            if (K32GetMappedFileNameW(GetCurrentProcess(), reinterpret_cast<void*>(a.base), path,
                                      MAX_PATH) != 0) {
                a.name = BaseName(Utf8Of(path));
            }
            if (a.name.empty()) {
                a.name = "section";
            }
            result.mapped.push_back(std::move(a));
            return;
        }
        if (std::binary_search(heaps.begin(), heaps.end(), a.base)) {
            result.heaps.Add(a.committed, a.resident);
            ++result.heaps.count;
            if (a.base == crt_heap) {
                result.crt_heap.Add(a.committed, a.resident);
            }
            return;
        }
        if (current.exec) {
            result.exec.push_back(std::move(a));
            return;
        }
        if (current.guard) {
            result.stacks.Add(a.committed, a.resident);
            ++result.stacks.count;
            return;
        }
        // Windows heaps keep their memory in allocations of their own past the first segment:
        // further segments (the CRT heap's grow to 1 GiB reservations) start with a segment header
        // (signature 0xffeeffee at +0x10, the heap at +0x28), and blocks over ~508 KiB get one
        // each, starting with their size: CommitSize at +0x20, ReserveSize at +0x28 (x64 layout).
        // Read with ReadProcessMemory: an allocation freed since the walk saw it fails the read
        // instead of faulting.
        if (current.first_rw) {
            struct Header {
                u64 words[6];
            } header{};
            SIZE_T read = 0;
            if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(a.base),
                                  &header, sizeof(header), &read) &&
                read == sizeof(header)) {
                const u32 signature = static_cast<u32>(header.words[2]);
                const u64 heap = header.words[5];
                if (signature == 0xffeeffee &&
                    std::binary_search(heaps.begin(), heaps.end(), static_cast<uintptr_t>(heap))) {
                    (heap == crt_heap ? result.crt_segments : result.other_segments)
                        .Add(a.committed, a.resident);
                    ++(heap == crt_heap ? result.crt_segments : result.other_segments).count;
                    return;
                }
                const u64 commit = header.words[4];
                const u64 reserve = header.words[5];
                constexpr u64 Slack = 0x10000;
                if (commit != 0 && commit <= reserve && reserve <= a.span + Slack &&
                    reserve + Slack >= a.span && commit <= a.committed + Slack &&
                    commit + Slack >= a.committed) {
                    result.big_blocks.push_back(std::move(a));
                    return;
                }
            }
        }
        if (const std::string* label = matcher.Take(a.span)) {
            Amount& matched = result.matched[*label];
            matched.Add(a.committed, a.resident);
            ++matched.count;
            return;
        }
        Amount& bucket = result.buckets[BucketOf(a.span)];
        bucket.Add(a.committed, a.resident);
        ++bucket.count;
        result.other.push_back(std::move(a));
    };

    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    auto address = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const auto max_address = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi{};
    while (address < max_address &&
           VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const auto begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = begin + mbi.RegionSize;
        if (end <= address) {
            break;
        }
        address = end;
        ++result.regions;
        if (mbi.State == MEM_FREE) {
            continue;
        }
        const auto allocation_base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
        if (!have_current || current.allocation.base != allocation_base) {
            flush();
            current = Current{};
            current.allocation.base = allocation_base;
            current.type = mbi.Type;
            have_current = true;
        }
        current.allocation.span = end - allocation_base;
        if (mbi.State == MEM_RESERVE) {
            result.reserved += mbi.RegionSize;
            continue;
        }
        constexpr DWORD Executable =
            PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & PAGE_GUARD) != 0) {
            current.guard = true;
        }
        if ((mbi.Protect & Executable) != 0) {
            current.exec = true;
        }
        if (begin == allocation_base && (mbi.Protect & 0xff) == PAGE_READWRITE &&
            (mbi.Protect & PAGE_GUARD) == 0) {
            current.first_rw = true;
        }
        if (mbi.RegionSize > current.largest_region) {
            current.largest_region = mbi.RegionSize;
            current.allocation.protect = mbi.Protect;
        }
        const auto [untagged, untagged_resident] =
            SplitTagged(tags, begin, end, result, resident_of);
        current.allocation.committed += untagged;
        current.allocation.resident += untagged_resident;
    }
    flush();
    result.walk_ms = static_cast<u32>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - start)
                                          .count());

    if (mode == 2) {
        // Locks each heap for its summary (EDEN_NX2_MEM_HEAPS measured ~250 ms for a 2.5 GB CRT
        // heap): only for a memory hunt.
        std::vector<std::pair<u64, std::string>> rows;
        for (const HANDLE heap : heap_handles) {
            HEAP_SUMMARY summary{};
            summary.cb = sizeof(summary);
            if (!HeapSummary(heap, 0, &summary)) {
                continue;
            }
            const auto base = reinterpret_cast<uintptr_t>(heap);
            rows.emplace_back(summary.cbCommitted,
                              fmt::format("{}{:x} {}/{}", base == crt_heap ? "CRT " : "", base,
                                          MiBOf(summary.cbCommitted),
                                          MiBOf(summary.cbAllocated)));
        }
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t i = 0; i < rows.size() && i < 12; ++i) {
            result.heap_text += (i == 0 ? "" : ", ") + rows[i].second;
        }
    }
    static bool histogram_done = false;
    if (mode == 4 && !histogram_done) {
        histogram_done = true;
        result.heap_text = CrtHeapHistogram(reinterpret_cast<HANDLE>(crt_heap));
    }
}
#elif defined(__linux__)
void Walk(int mode, Result& result) {
    const auto start = std::chrono::steady_clock::now();
    static_cast<void>(mode);
    const std::vector<TagEntry> tags = CopyTags();
    std::ifstream file{"/proc/self/smaps"};
    std::string line;
    struct Mapping {
        uintptr_t begin{};
        uintptr_t end{};
        std::string perms;
        std::string path;
        u64 rss{};
        bool valid{};
    };
    Mapping mapping;
    const auto flush = [&] {
        if (!mapping.valid) {
            return;
        }
        mapping.valid = false;
        ++result.regions;
        const u64 size = mapping.end - mapping.begin;
        // Rss of a mapping is not split by address: a tagged mapping goes to its tag whole.
        for (const TagEntry& tag : tags) {
            if (mapping.begin >= tag.base && mapping.begin < tag.base + tag.size) {
                result.tagged[tag.name].Add(size, mapping.rss);
                return;
            }
        }
        Allocation a{.base = mapping.begin, .span = size, .committed = size,
                     .resident = mapping.rss, .protect = 0, .name = {}};
        const std::string& path = mapping.path;
        const bool exec = mapping.perms.find('x') != std::string::npos;
        if (path.find(".so") != std::string::npos) {
            const size_t slash = path.find_last_of('/');
            a.name = slash == std::string::npos ? path : path.substr(slash + 1);
            result.images.push_back(std::move(a));
        } else if (path == "[heap]") {
            result.heaps.Add(size, mapping.rss);
            result.crt_heap.Add(size, mapping.rss);
            ++result.heaps.count;
        } else if (path.rfind("[stack", 0) == 0) {
            result.stacks.Add(size, mapping.rss);
            ++result.stacks.count;
        } else if (!path.empty() && path[0] != '[') {
            const size_t slash = path.find_last_of('/');
            a.name = slash == std::string::npos ? path : path.substr(slash + 1);
            result.mapped.push_back(std::move(a));
        } else if (exec) {
            result.exec.push_back(std::move(a));
        } else if (mapping.perms.rfind("---", 0) == 0) {
            result.reserved += size;
        } else {
            Amount& bucket = result.buckets[BucketOf(size)];
            bucket.Add(size, mapping.rss);
            ++bucket.count;
            result.other.push_back(std::move(a));
        }
    };
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }
        const char first = line[0];
        const bool header = (first >= '0' && first <= '9') || (first >= 'a' && first <= 'f');
        const size_t dash = line.find('-');
        if (header && dash != std::string::npos && dash < line.find(' ')) {
            flush();
            std::istringstream in{line};
            std::string range, offset, device, inode;
            in >> range >> mapping.perms >> offset >> device >> inode;
            std::getline(in, mapping.path);
            mapping.path.erase(0, mapping.path.find_first_not_of(' '));
            mapping.begin = std::strtoull(range.c_str(), nullptr, 16);
            mapping.end = std::strtoull(range.c_str() + dash + 1, nullptr, 16);
            mapping.rss = 0;
            mapping.valid = true;
        } else if (line.rfind("Rss:", 0) == 0) {
            mapping.rss = std::strtoull(line.c_str() + 4, nullptr, 10) * 1024;
            result.ws_pages += mapping.rss / 4096;
        }
    }
    flush();
    result.walk_ms = static_cast<u32>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - start)
                                          .count());
}
#else
void Walk(int, Result&) {}
#endif

std::string Top(std::vector<Allocation>& list, size_t count, bool with_base) {
    std::sort(list.begin(), list.end(),
              [](const Allocation& a, const Allocation& b) { return a.committed > b.committed; });
    std::string text;
    for (size_t i = 0; i < list.size() && i < count; ++i) {
        const Allocation& a = list[i];
        text += i == 0 ? "" : ", ";
        if (with_base) {
            text += fmt::format("{:x} span {} {}/{} {}", a.base, MiBOf(a.span), MiBOf(a.committed),
                                MiBOf(a.resident), ProtectText(a.protect));
        } else {
            text += fmt::format("{} {}/{}", a.name, MiBOf(a.committed), MiBOf(a.resident));
        }
    }
    return text;
}

Amount Sum(const std::vector<Allocation>& list) {
    Amount sum{};
    for (const Allocation& a : list) {
        sum.Add(a.committed, a.resident);
        ++sum.count;
    }
    return sum;
}

/// Same-named images summed (a DLL loaded twice is rare; drivers load several of their own).
std::vector<Allocation> MergeByName(const std::vector<Allocation>& list) {
    std::map<std::string, Allocation> merged;
    for (const Allocation& a : list) {
        Allocation& m = merged[a.name];
        m.name = a.name;
        m.committed += a.committed;
        m.resident += a.resident;
    }
    std::vector<Allocation> out;
    for (auto& [name, a] : merged) {
        out.push_back(std::move(a));
    }
    return out;
}

void Census(int mode, const char* reason) {
    Result result;
    Walk(mode, result);
    const ProcessMemoryUsage usage = GetProcessMemoryUsage();
    std::string tagged;
    for (const auto& [name, amount] : result.tagged) {
        tagged += fmt::format("{}{} {}/{}", tagged.empty() ? "" : ", ", name,
                              MiBOf(amount.committed), MiBOf(amount.resident));
    }
    const Amount images = Sum(result.images);
    const Amount mapped = Sum(result.mapped);
    const Amount exec = Sum(result.exec);
    const Amount other = Sum(result.other);
    const Amount big_blocks = Sum(result.big_blocks);
    u64 attributed = images.resident + mapped.resident + exec.resident + other.resident +
                     result.heaps.resident + result.stacks.resident + big_blocks.resident +
                     result.crt_segments.resident + result.other_segments.resident;
    for (const auto& [name, amount] : result.tagged) {
        attributed += amount.resident;
    }
    std::string matched;
    for (const auto& [name, amount] : result.matched) {
        matched += fmt::format("{}{} {}/{} ({})", matched.empty() ? "" : ", ", name,
                               MiBOf(amount.committed), MiBOf(amount.resident), amount.count);
        attributed += amount.resident;
    }
    LOG_INFO(HW_Memory,
             "Memory census ({}, FRITCHII_MEM_CENSUS={}): {} regions walked in {} ms (working set "
             "query {} ms, {} MiB of pages); working set {} MiB (peak {}), private {}, private "
             "commit {} (peak {}), reserved {} GiB; committed/resident MiB: {}; images {}/{} ({}); "
             "mapped other {}/{} ({}); exec private {}/{} ({} allocations); heaps {}/{} ({} first "
             "segments, CRT {}/{}); heap segments CRT {}/{} ({}), other heaps {}/{} ({}); heap "
             "big blocks {}/{} ({}); size matches: {}; stacks {}/{} ({}); other private {}/{} ({} "
             "allocations); resident not in any of them {}",
             reason, mode, result.regions, result.walk_ms, result.query_ms,
             MiBOf(result.ws_pages * 4096), MiBOf(usage.working_set),
             MiBOf(usage.peak_working_set), MiBOf(usage.private_working_set),
             MiBOf(usage.private_bytes), MiBOf(usage.peak_private_bytes), result.reserved >> 30,
             tagged.empty() ? "nothing tagged" : tagged, MiBOf(images.committed),
             MiBOf(images.resident), images.count, MiBOf(mapped.committed), MiBOf(mapped.resident),
             mapped.count, MiBOf(exec.committed), MiBOf(exec.resident), exec.count,
             MiBOf(result.heaps.committed), MiBOf(result.heaps.resident), result.heaps.count,
             MiBOf(result.crt_heap.committed), MiBOf(result.crt_heap.resident),
             MiBOf(result.crt_segments.committed), MiBOf(result.crt_segments.resident),
             result.crt_segments.count, MiBOf(result.other_segments.committed),
             MiBOf(result.other_segments.resident), result.other_segments.count,
             MiBOf(big_blocks.committed), MiBOf(big_blocks.resident), big_blocks.count,
             matched.empty() ? "none" : matched, MiBOf(result.stacks.committed), MiBOf(result.stacks.resident), result.stacks.count,
             MiBOf(other.committed), MiBOf(other.resident), other.count,
             static_cast<s64>(result.ws_pages * 4096 - (std::min)(result.ws_pages * 4096, attributed)) >> 20);
    std::vector<Allocation> merged_images = MergeByName(result.images);
    std::vector<Allocation> merged_mapped = MergeByName(result.mapped);
    LOG_INFO(HW_Memory, "Memory census ({}) images, committed/resident MiB: {}", reason,
             Top(merged_images, 14, false));
    LOG_INFO(HW_Memory, "Memory census ({}) mapped other: {}", reason,
             Top(merged_mapped, 6, false));
    std::string buckets;
    for (size_t i = 0; i < result.buckets.size(); ++i) {
        const Amount& b = result.buckets[i];
        buckets += fmt::format("{}{} {}/{} ({})", i == 0 ? "" : ", ", BucketNames[i],
                               MiBOf(b.committed), MiBOf(b.resident), b.count);
    }
    LOG_INFO(HW_Memory,
             "Memory census ({}) private: exec (JIT code caches, driver code) {}; other by "
             "reservation size {}; largest other: {}; largest heap big blocks: {}",
             reason, Top(result.exec, 10, true), buckets, Top(result.other, 10, true),
             Top(result.big_blocks, 8, true));
    std::string extra = Nx2Hitch::JitCompileCensus();
    std::function<std::string()> sampler;
    {
        std::scoped_lock lock{g_tag_mutex};
        sampler = g_sampler;
    }
    if (sampler) {
        extra += "; " + sampler();
    }
    if (!result.heap_text.empty()) {
        extra += "; heaps (committed/allocated MiB, or the CRT heap walk): " + result.heap_text;
    }
    LOG_INFO(HW_Memory, "Memory census ({}) more: {}", reason, extra);
}

u64 WorkingSetNow() {
#ifdef __linux__
    std::ifstream statm{"/proc/self/statm"};
    u64 size = 0;
    u64 resident = 0;
    statm >> size >> resident;
    return resident * 4096;
#else
    return GetProcessMemoryUsage().working_set;
#endif
}

void Run(std::stop_token stop, int mode) {
    using namespace std::chrono_literals;
    std::mutex mutex;
    std::condition_variable_any wake;
    constexpr u64 RiseBytes = u64{512} << 20;
    auto last_regular = std::chrono::steady_clock::now();
    auto last_peak = last_regular - 1h;
    u64 last_census_ws = WorkingSetNow();
    u64 highest_ws = last_census_ws;
    while (true) {
        {
            std::unique_lock lock{mutex};
            wake.wait_for(lock, stop, 1s, [] { return false; });
        }
        if (stop.stop_requested()) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const u64 ws = WorkingSetNow();
        const bool climbing = ws > highest_ws;
        highest_ws = (std::max)(highest_ws, ws);
        // A load peak: the working set rose 512 MiB past the last census and is the highest yet
        // (one every 5 s at most, so a long climb gets a few).
        if (climbing && ws >= last_census_ws + RiseBytes && now - last_peak >= 5s) {
            last_peak = now;
            Census(mode, "peak");
            last_census_ws = WorkingSetNow();
            continue;
        }
        if (now - last_regular >= 30s) {
            last_regular = now;
            Census(mode, "30 s");
            last_census_ws = WorkingSetNow();
        }
    }
}

} // Anonymous namespace

int Mode() {
    static const int mode = [] {
        const char* const value = std::getenv("FRITCHII_MEM_CENSUS");
        return value != nullptr ? std::atoi(value) : 0;
    }();
    return mode;
}

void Tag(const char* name, const void* base, u64 size) {
    if (Mode() == 0 || base == nullptr || size == 0) {
        return;
    }
    const TagEntry entry{name, reinterpret_cast<uintptr_t>(base), size};
    std::scoped_lock lock{g_tag_mutex};
    const auto it = std::lower_bound(
        g_tags.begin(), g_tags.end(), entry.base,
        [](const TagEntry& tag, uintptr_t value) { return tag.base < value; });
    if (it != g_tags.end() && it->base == entry.base) {
        *it = entry;
        return;
    }
    g_tags.insert(it, entry);
}

void Untag(const void* base) {
    if (Mode() == 0) {
        return;
    }
    std::scoped_lock lock{g_tag_mutex};
    std::erase_if(g_tags, [base](const TagEntry& tag) {
        return tag.base == reinterpret_cast<uintptr_t>(base);
    });
}

void SetGuestSampler(std::function<std::string()> sampler) {
    std::scoped_lock lock{g_tag_mutex};
    g_sampler = std::move(sampler);
}

void SetSizeSource(const char* label, std::function<std::vector<u64>()> source) {
    std::scoped_lock lock{g_tag_mutex};
    std::erase_if(g_size_sources, [label](const auto& entry) {
        return std::string_view{entry.first} == label;
    });
    if (source) {
        g_size_sources.emplace_back(label, std::move(source));
    }
}

void Start() {
    const int mode = Mode();
    if (mode == 0) {
        return;
    }
    std::scoped_lock lock{g_thread_mutex};
    if (g_thread.joinable()) {
        return;
    }
    LOG_INFO(HW_Memory,
             "Memory census on (FRITCHII_MEM_CENSUS={}): every 30 s and at working set peaks",
             mode);
    g_thread = std::jthread{[mode](std::stop_token stop) {
        Common::SetCurrentThreadName("MemCensus");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
        Run(stop, mode);
    }};
}

void Stop() {
    std::jthread thread;
    {
        std::scoped_lock lock{g_thread_mutex};
        thread = std::move(g_thread);
    }
    if (thread.joinable()) {
        thread.request_stop();
        thread.join();
    }
}

} // namespace MemCensus

} // namespace Common
