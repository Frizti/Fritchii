// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "common/nx2_sample_profile.h"

#ifdef _WIN32
#include <array>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <thread>

#include <windows.h>
#include <tlhelp32.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#elif defined(__linux__) && !defined(__ANDROID__)
#include <array>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <ctime>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#endif

namespace Common::Nx2SampleProfile {

namespace {

struct Config {
    unsigned hz{};
    unsigned delay_seconds{};
    unsigned seconds{20};
    std::vector<std::string> threads;
};

unsigned ReadUnsigned(const char* variable, unsigned fallback) {
    const char* const value = std::getenv(variable);
    return value != nullptr ? static_cast<unsigned>(std::strtoul(value, nullptr, 10)) : fallback;
}

const Config& GetConfig() {
    static const Config config = [] {
        Config result{
            .hz = ReadUnsigned("EDEN_NX2_SAMPLE_PROFILE", 0),
            .delay_seconds = ReadUnsigned("EDEN_NX2_SAMPLE_DELAY", 0),
            .seconds = ReadUnsigned("EDEN_NX2_SAMPLE_SECONDS", 20),
        };
        const char* const list = std::getenv("EDEN_NX2_SAMPLE_THREADS");
        std::string_view rest = list != nullptr ? list : "GPU,GPUBack,VulkanWorker";
        while (!rest.empty()) {
            const size_t comma = rest.find(',');
            if (const std::string_view item = rest.substr(0, comma); !item.empty()) {
                result.threads.emplace_back(item);
            }
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        }
        return result;
    }();
    return config;
}

bool Selected(std::string_view name) {
    for (const std::string& pattern : GetConfig().threads) {
        if (!pattern.empty() && pattern.back() == '*'
                ? name.starts_with(std::string_view{pattern}.substr(0, pattern.size() - 1))
                : name == pattern) {
            return true;
        }
    }
    return false;
}

#ifdef _WIN32

// Record kinds of the sample file (tools/nx2/sample_report.py parses it).
constexpr std::uint8_t KindSample = 1;
constexpr std::uint8_t KindThread = 2;
constexpr std::uint8_t KindModule = 3;
constexpr std::uint8_t KindEnd = 0xff;
constexpr size_t MaxThreads = 32;
constexpr size_t MaxDepth = 32;

struct SampledThread {
    HANDLE handle{};
    std::string name;
};

std::mutex threads_mutex;
std::array<SampledThread, MaxThreads> sampled_threads;
std::atomic<size_t> num_threads{};

template <typename T>
void Put(std::ofstream& file, T value) {
    file.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void PutString(std::ofstream& file, std::string_view text) {
    Put(file, static_cast<std::uint16_t>(text.size()));
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string Utf8(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string result(size > 0 ? size - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
}

void WriteModules(std::ofstream& file) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    MODULEENTRY32W entry{.dwSize = sizeof(MODULEENTRY32W)};
    for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry)) {
        Put(file, KindModule);
        Put(file, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(entry.modBaseAddr)));
        Put(file, static_cast<std::uint32_t>(entry.modBaseSize));
        PutString(file, Utf8(entry.szExePath));
    }
    CloseHandle(snapshot);
}

/// Unwinds a suspended thread's stack. Allocates nothing: the thread may hold the heap lock.
size_t WalkUnguarded(CONTEXT& context, std::array<std::uint64_t, MaxDepth>& frames,
                     size_t& depth) {
    while (depth < MaxDepth && context.Rip != 0) {
        frames[depth++] = context.Rip;
        DWORD64 image_base{};
        const PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (function == nullptr) {
            // A leaf without unwind data has its return address on top of the stack. Past the
            // first frame this is code without unwind data (JIT): stop there.
            if (depth > 1) {
                break;
            }
            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
            context.Rsp += sizeof(DWORD64);
            continue;
        }
        PVOID handler_data{};
        DWORD64 establisher_frame{};
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                         &handler_data, &establisher_frame, nullptr);
    }
    return depth;
}

/// A thread stopped in driver or JIT code can leave the unwinder a garbage stack pointer: it then
/// faults inside ntdll and took the whole emulator down (twice in Ocarina of Time sessions). Keep
/// the frames found up to the fault instead.
size_t Walk(CONTEXT& context, std::array<std::uint64_t, MaxDepth>& frames) {
    size_t depth = 0;
#ifdef _MSC_VER
    __try {
        WalkUnguarded(context, frames, depth);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#else
    WalkUnguarded(context, frames, depth);
#endif
    return depth;
}

void SamplerMain() {
    const Config& config = GetConfig();
    Sleep(config.delay_seconds * 1000);

    const auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) /
                      ("nx2_samples_" + std::to_string(GetCurrentProcessId()) + ".bin");
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    if (!file) {
        LOG_ERROR(Common, "Sample profile: can't open {}", path.string());
        return;
    }
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    file.write("NX2SMPL1", 8);
    Put(file, std::uint32_t{1});
    Put(file, static_cast<std::uint32_t>(config.hz));
    Put(file, static_cast<std::uint64_t>(frequency.QuadPart));
    WriteModules(file);

    // A high resolution timer reaches 2 kHz; without it (before Windows 10 1803) Sleep(1) runs
    // at the system timer resolution.
    constexpr DWORD HighResolution = 0x2; // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
    const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, HighResolution, TIMER_ALL_ACCESS);
    const LONGLONG period_100ns = -static_cast<LONGLONG>(10'000'000 / config.hz);

    LARGE_INTEGER start{};
    QueryPerformanceCounter(&start);
    const LONGLONG end = start.QuadPart + static_cast<LONGLONG>(config.seconds) * frequency.QuadPart;
    size_t threads_written = 0;
    std::uint64_t samples = 0;
    std::array<std::uint64_t, MaxDepth> frames{};
    for (LARGE_INTEGER now = start; now.QuadPart < end; QueryPerformanceCounter(&now)) {
        const size_t count = num_threads.load(std::memory_order_acquire);
        for (; threads_written < count; ++threads_written) {
            Put(file, KindThread);
            Put(file, static_cast<std::uint8_t>(threads_written));
            PutString(file, sampled_threads[threads_written].name);
        }
        for (size_t index = 0; index < count; ++index) {
            const HANDLE thread = sampled_threads[index].handle;
            if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
                continue;
            }
            alignas(16) CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            const size_t depth = GetThreadContext(thread, &context) ? Walk(context, frames) : 0;
            ResumeThread(thread);
            if (depth == 0) {
                continue;
            }
            Put(file, KindSample);
            Put(file, static_cast<std::uint8_t>(index));
            Put(file, static_cast<std::uint8_t>(depth));
            Put(file, static_cast<std::uint8_t>(0));
            Put(file, static_cast<std::uint64_t>(now.QuadPart));
            file.write(reinterpret_cast<const char*>(frames.data()),
                       static_cast<std::streamsize>(depth * sizeof(std::uint64_t)));
            ++samples;
        }
        if (timer != nullptr) {
            LARGE_INTEGER due{.QuadPart = period_100ns};
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(1);
        }
    }
    Put(file, KindEnd);
    file.close();
    if (timer != nullptr) {
        CloseHandle(timer);
    }
    LOG_INFO(Common, "Sample profile: {} samples of {} threads at {} Hz written to {}", samples,
             threads_written, config.hz, path.string());
}

#endif

#if defined(__linux__) && !defined(__ANDROID__)

// Record kinds of the sample file (tools/nx2/sample_report.py parses it), as on Windows.
constexpr std::uint8_t KindSample = 1;
constexpr std::uint8_t KindThread = 2;
constexpr std::uint8_t KindModule = 3;
constexpr std::uint8_t KindEnd = 0xff;
constexpr size_t MaxThreads = 32;
// SIGPROF: what sampling profilers use; handlers are installed with SA_RESTART.
constexpr int SampleSignal = SIGPROF;

struct SampledThread {
    pid_t tid{};
    std::string name;
};

std::mutex threads_mutex;
std::array<SampledThread, MaxThreads> sampled_threads;
std::atomic<size_t> num_threads{};

// One sample in flight: the sampler names the thread it signals, the handler of that thread (and
// only that one: a late signal of an earlier request finds another target) stores its PC.
std::atomic<pid_t> g_target{0};
std::atomic<std::uint64_t> g_pc{0};
std::atomic<bool> g_done{false};

pid_t CurrentTid() {
    return static_cast<pid_t>(syscall(SYS_gettid));
}

/// Async-signal-safe: reads the interrupted PC from the signal context and stores it.
void SampleHandler(int, siginfo_t*, void* raw_context) {
    const int saved_errno = errno;
    if (g_target.load(std::memory_order_acquire) == CurrentTid()) {
        const auto* context = static_cast<const ucontext_t*>(raw_context);
#if defined(__x86_64__)
        const auto pc = static_cast<std::uint64_t>(context->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
        const auto pc = static_cast<std::uint64_t>(context->uc_mcontext.pc);
#else
        const std::uint64_t pc = 0;
        (void)context;
#endif
        g_pc.store(pc, std::memory_order_relaxed);
        g_done.store(true, std::memory_order_release);
    }
    errno = saved_errno;
}

template <typename T>
void Put(std::ofstream& file, T value) {
    file.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void PutString(std::ofstream& file, std::string_view text) {
    Put(file, static_cast<std::uint16_t>(text.size()));
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::uint64_t NowNs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(now.tv_nsec);
}

/// Each mapped file as one module: from its lowest mapping (where file offset 0 is loaded, the
/// address ELF virtual addresses count from) to its highest end.
void WriteModules(std::ofstream& file) {
    std::ifstream maps{"/proc/self/maps"};
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> modules;
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream fields{line};
        std::string range, perms, offset, device, inode, path;
        fields >> range >> perms >> offset >> device >> inode;
        std::getline(fields >> std::ws, path);
        if (path.empty() || path.front() != '/') {
            continue;
        }
        const size_t dash = range.find('-');
        const std::uint64_t begin = std::stoull(range.substr(0, dash), nullptr, 16);
        const std::uint64_t end = std::stoull(range.substr(dash + 1), nullptr, 16);
        auto [it, inserted] = modules.try_emplace(path, begin, end);
        if (!inserted) {
            it->second.first = (std::min)(it->second.first, begin);
            it->second.second = (std::max)(it->second.second, end);
        }
    }
    for (const auto& [path, range] : modules) {
        Put(file, KindModule);
        Put(file, range.first);
        Put(file, static_cast<std::uint32_t>(range.second - range.first));
        PutString(file, path);
    }
}

void SamplerMain() {
    const Config& config = GetConfig();
    std::this_thread::sleep_for(std::chrono::seconds{config.delay_seconds});

    struct sigaction action{};
    action.sa_sigaction = SampleHandler;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(SampleSignal, &action, nullptr) != 0) {
        LOG_ERROR(Common, "Sample profile: can't install the SIGPROF handler");
        return;
    }
    const auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) /
                      ("nx2_samples_" + std::to_string(getpid()) + ".bin");
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    if (!file) {
        LOG_ERROR(Common, "Sample profile: can't open {}", path.string());
        return;
    }
    file.write("NX2SMPL1", 8);
    Put(file, std::uint32_t{1});
    Put(file, static_cast<std::uint32_t>(config.hz));
    Put(file, std::uint64_t{1'000'000'000}); // tick frequency: nanoseconds
    WriteModules(file);

    const std::uint64_t period = 1'000'000'000ULL / (std::max)(config.hz, 1u);
    const std::uint64_t start = NowNs();
    const std::uint64_t end = start + static_cast<std::uint64_t>(config.seconds) * 1'000'000'000ULL;
    const pid_t process = getpid();
    size_t threads_written = 0;
    std::uint64_t samples = 0;
    std::uint64_t next = start;
    for (std::uint64_t now = start; now < end; now = NowNs()) {
        const size_t count = num_threads.load(std::memory_order_acquire);
        for (; threads_written < count; ++threads_written) {
            Put(file, KindThread);
            Put(file, static_cast<std::uint8_t>(threads_written));
            PutString(file, sampled_threads[threads_written].name);
        }
        for (size_t index = 0; index < count; ++index) {
            const pid_t tid = sampled_threads[index].tid;
            g_done.store(false, std::memory_order_relaxed);
            g_target.store(tid, std::memory_order_release);
            if (syscall(SYS_tgkill, process, tid, SampleSignal) != 0) {
                continue;
            }
            // The handler runs as soon as the thread is scheduled; a thread that doesn't get
            // there within 2 ms is skipped this round.
            const std::uint64_t give_up = NowNs() + 2'000'000;
            while (!g_done.load(std::memory_order_acquire) && NowNs() < give_up) {
            }
            g_target.store(0, std::memory_order_release);
            if (!g_done.load(std::memory_order_acquire)) {
                continue;
            }
            const std::uint64_t pc = g_pc.load(std::memory_order_relaxed);
            if (pc == 0) {
                continue;
            }
            Put(file, KindSample);
            Put(file, static_cast<std::uint8_t>(index));
            Put(file, static_cast<std::uint8_t>(1));
            Put(file, static_cast<std::uint8_t>(0));
            Put(file, now);
            Put(file, pc);
            ++samples;
        }
        next += period;
        const std::uint64_t after = NowNs();
        if (next > after) {
            const timespec wait{static_cast<time_t>((next - after) / 1'000'000'000ULL),
                                static_cast<long>((next - after) % 1'000'000'000ULL)};
            nanosleep(&wait, nullptr);
        } else {
            next = after;
        }
    }
    Put(file, KindEnd);
    file.close();
    LOG_INFO(Common, "Sample profile: {} samples of {} threads at {} Hz written to {}", samples,
             threads_written, config.hz, path.string());
}

#endif

} // namespace

bool Requested() {
    return GetConfig().hz != 0;
}

void OnThreadNamed(const char* name) {
    if (!Requested() || !Selected(name)) {
        return;
    }
#ifdef _WIN32
    HANDLE handle{};
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                         FALSE, 0)) {
        return;
    }
    {
        std::scoped_lock lock{threads_mutex};
        const size_t index = num_threads.load(std::memory_order_relaxed);
        if (index == MaxThreads) {
            CloseHandle(handle);
            return;
        }
        sampled_threads[index] = {handle, name};
        num_threads.store(index + 1, std::memory_order_release);
    }
    static std::once_flag started;
    std::call_once(started, [] { std::thread{SamplerMain}.detach(); });
#elif defined(__linux__) && !defined(__ANDROID__)
    {
        std::scoped_lock lock{threads_mutex};
        const size_t index = num_threads.load(std::memory_order_relaxed);
        if (index == MaxThreads) {
            return;
        }
        sampled_threads[index] = {CurrentTid(), name};
        num_threads.store(index + 1, std::memory_order_release);
    }
    static std::once_flag started;
    std::call_once(started, [] { std::thread{SamplerMain}.detach(); });
#endif
}

} // namespace Common::Nx2SampleProfile
