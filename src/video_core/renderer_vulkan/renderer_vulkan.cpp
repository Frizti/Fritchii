// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fmt/ranges.h>

#include "common/fs/fs.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include <ranges>
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core_timing.h"
#include "core/frontend/graphics_context.h"
#include "core/nvn_hook.h"
#include "video_core/capture.h"
#include "video_core/gpu.h"
#include "video_core/shader_notify.h"
#include "video_core/present.h"
#include "video_core/render_trace.h"
#include "video_core/renderer_vulkan/present/util.h"
#ifdef HAS_RESHADE
#include "video_core/post_processing/fx_chain.h"
#endif
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_blit_screen.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/textures/decoders.h"
#include "video_core/vulkan_common/vulkan_debug_callback.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_surface.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/one_follower.h"
#ifdef EGLANTINE_STREAMLINE
#include "video_core/renderer_vulkan/vk_streamline_dlss.h"
#include "video_core/vulkan_common/streamline.h"
#endif
#ifdef __ANDROID__
#include <jni.h>
#endif
#ifdef LICHII_NVN_DIRECT
#include "video_core/nvn_direct/nvn_direct.h"
#include <chrono>
#include <mutex>
#ifndef _WIN32
#include <dlfcn.h>
#include <unistd.h>
#endif
#endif
namespace Vulkan {
namespace {

constexpr VkExtent2D CaptureImageSize{
    .width = VideoCore::Capture::LinearWidth,
    .height = VideoCore::Capture::LinearHeight,
};

constexpr VkExtent3D CaptureImageExtent{
    .width = VideoCore::Capture::LinearWidth,
    .height = VideoCore::Capture::LinearHeight,
    .depth = VideoCore::Capture::LinearDepth,
};

constexpr VkFormat CaptureFormat = VK_FORMAT_A8B8G8R8_UNORM_PACK32;

std::string GetReadableVersion(u32 version) {
    return fmt::format("{}.{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
                       VK_VERSION_PATCH(version));
}

std::string GetDriverVersion(const Device& device) {
    // Extracted from
    // https://github.com/SaschaWillems/vulkan.gpuinfo.org/blob/5dddea46ea1120b0df14eef8f15ff8e318e35462/functions.php#L308-L314
    const u32 version = device.GetDriverVersion();

    if (device.GetDriverID() == VK_DRIVER_ID_NVIDIA_PROPRIETARY) {
        const u32 major = (version >> 22) & 0x3ff;
        const u32 minor = (version >> 14) & 0x0ff;
        const u32 secondary = (version >> 6) & 0x0ff;
        const u32 tertiary = version & 0x003f;
        return fmt::format("{}.{}.{}.{}", major, minor, secondary, tertiary);
    }
    if (device.GetDriverID() == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS) {
        const u32 major = version >> 14;
        const u32 minor = version & 0x3fff;
        return fmt::format("{}.{}", major, minor);
    }
    return GetReadableVersion(version);
}

std::string BuildCommaSeparatedExtensions(
    const std::set<std::string, std::less<>>& available_extensions) {
    return fmt::format("{}", fmt::join(available_extensions, ","));
}

#ifdef LICHII_NVN_DIRECT
#ifdef _WIN32
// Under WDDM NVML has no figure for a process (nvml.h: usedGpuMemory is NVML_VALUE_NOT_AVAILABLE
// there, as Windows' kernel driver manages the memory): the memory budget extension's
class NvmlProcessMemory {
public:
    explicit NvmlProcessMemory(const Device&) {
        LOG_INFO(Render_Vulkan, "NVN2 direct mode: VRAM use from the memory budget extension (NVML "
                                "has no per-process figure under WDDM)");
    }
    u64 Get() {
        return 0;
    }
};
#else
/// NVN2 direct mode: NVML's figure for this process's video memory on the
/// device (NVIDIA only; 0 elsewhere or when NVML has no entry for it). VK_EXT_memory_budget's heap
/// usage leaves out what the driver keeps for itself: Mario Kart World's race showed 8.8 GiB of
/// heap usage while NVML had the process at 12.1 GiB, so a budget on the heap usage
/// never held the cap. Read at most every 250 ms (an NVML process query takes about a millisecond).
class NvmlProcessMemory {
public:
    NvmlProcessMemory(const NvmlProcessMemory&) = delete;
    NvmlProcessMemory& operator=(const NvmlProcessMemory&) = delete;
    ~NvmlProcessMemory() {
        if (nvml_initialized) {
            if (const auto shutdown = reinterpret_cast<int (*)()>(dlsym(library, "nvmlShutdown"))) {
                shutdown();
            }
        }
        if (library != nullptr) {
            dlclose(library);
        }
    }

    explicit NvmlProcessMemory(const Device& device) {
        library = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr) { // (NixOS keeps the driver's libraries here)
            library = dlopen("/run/opengl-driver/lib/libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
        }
        if (library == nullptr) {
            LOG_INFO(Render_Vulkan, "NVN2 direct mode: VRAM use from the memory budget extension (no NVML)");
            return;
        }
        const auto load = [this](const char* name) { return dlsym(library, name); };
        const auto init = reinterpret_cast<int (*)()>(load("nvmlInit_v2"));
        const auto count = reinterpret_cast<int (*)(unsigned*)>(load("nvmlDeviceGetCount_v2"));
        const auto by_index = reinterpret_cast<int (*)(unsigned, void**)>(load("nvmlDeviceGetHandleByIndex_v2"));
        const auto uuid_of = reinterpret_cast<int (*)(void*, char*, unsigned)>(load("nvmlDeviceGetUUID"));
        processes = reinterpret_cast<int (*)(void*, unsigned*, ProcessInfo*)>(
            load("nvmlDeviceGetGraphicsRunningProcesses_v3"));
        unsigned devices = 0;
        nvml_initialized = init && count && by_index && uuid_of && processes && init() == 0;
        if (!nvml_initialized || count(&devices) != 0) {
            processes = nullptr;
            return;
        }
        // The NVML device with the Vulkan device's UUID ("GPU-" and the 16 bytes as 8-4-4-4-12).
        VkPhysicalDeviceIDProperties id{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                               .pNext = &id};
        device.GetPhysical().GetProperties2(properties);
        std::string want = "GPU-";
        for (size_t i = 0; i < VK_UUID_SIZE; ++i) {
            want += fmt::format("{:02x}", id.deviceUUID[i]);
            if (i == 3 || i == 5 || i == 7 || i == 9) {
                want += '-';
            }
        }
        for (unsigned index = 0; index < devices; ++index) {
            void* handle = nullptr;
            std::array<char, 96> uuid{};
            if (by_index(index, &handle) == 0 && uuid_of(handle, uuid.data(), 96) == 0 &&
                want == uuid.data()) {
                nvml_device = handle;
            }
        }
        LOG_INFO(Render_Vulkan, "NVN2 direct mode: VRAM use {} (NVML device {})",
                 nvml_device ? "from NVML for this process" : "from the memory budget extension",
                 nvml_device ? want : "not found");
    }

    /// Bytes, 0 when not known.
    u64 Get() {
        if (nvml_device == nullptr) {
            return 0;
        }
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock{mutex};
        if (now - read_at < std::chrono::milliseconds{250}) {
            return value;
        }
        read_at = now;
        std::array<ProcessInfo, 64> table{};
        unsigned count = static_cast<unsigned>(table.size());
        value = 0;
        if (processes(nvml_device, &count, table.data()) == 0) {
            const unsigned self = static_cast<unsigned>(getpid());
            for (unsigned i = 0; i < count && i < table.size(); ++i) {
                if (table[i].pid == self) {
                    value = table[i].used;
                }
            }
        }
        return value;
    }

private:
    struct ProcessInfo { // nvmlProcessInfo_t (the _v3 calls)
        unsigned pid;
        unsigned long long used;
        unsigned gpu_instance, compute_instance;
    };
    void* library{};
    bool nvml_initialized{};
    void* nvml_device{};
    int (*processes)(void*, unsigned*, ProcessInfo*){};
    std::mutex mutex;
    std::chrono::steady_clock::time_point read_at{};
    u64 value{};
};

#endif

} // Anonymous namespace

/// The device memory figures the replay reads (nvn_direct_device.memory_usage / memory_budget).
struct NvnDirectMemory {
    const Device* device;
    NvmlProcessMemory nvml;
    bool owns_cache_dir{};

    ~NvnDirectMemory() {
        if (!owns_cache_dir) {
            return;
        }
#ifdef _WIN32
        _putenv_s("DM_TEMP", "");
#else
        unsetenv("DM_TEMP");
#endif
    }
};

namespace {

/// NVN2 direct mode: the replay runs on this device. Its NVN queues take the
/// graphics family's queues from 1 on, its own work (uploads, sparse binds) queue 0 with Lichii; every
/// submission of the replay holds the scheduler's submit lock. Returns whether the replay is up.
bool AdoptNvnDirect(const vk::InstanceDispatch& dld, VkInstance instance, const Device& device,
                    Scheduler& scheduler, NvnDirectAstc* astc, NvnDirectShaders& shaders,
                    NvnDirectMemory& memory_source, VideoCore::ShaderNotify& shader_notify) {
    std::vector<const char*> extensions;
    for (const std::string& name : device.GetLoadedExtensions()) {
        extensions.push_back(name.c_str());
    }
    const std::span<const VkQueue> queues = device.GetNvnDirectQueues();
    nvn_direct_device adopted{
        .get_instance_proc_addr = dld.vkGetInstanceProcAddr,
        .instance = instance,
        .physical = device.GetPhysical(),
        .device = *device.GetLogical(),
        .queue_family = device.GetGraphicsFamily(),
        .queue_count = static_cast<uint32_t>(queues.size()),
        .queues = {},
        .extensions = extensions.data(),
        .extension_count = static_cast<uint32_t>(extensions.size()),
        .queue_lock = [](void* mutex) { static_cast<std::mutex*>(mutex)->lock(); },
        .queue_unlock = [](void* mutex) { static_cast<std::mutex*>(mutex)->unlock(); },
        .queue_context = &scheduler.submit_mutex,
        .astc_to_bc = astc != nullptr ? &NvnDirectAstc::Record : nullptr,
        .astc_flush = astc != nullptr ? &NvnDirectAstc::Flush : nullptr,
        .astc_context = astc,
        .astc_opaque = astc != nullptr ? &NvnDirectAstc::IsOpaque : nullptr,
        // NVML's figure when there is one (it counts what the driver keeps for itself), else the
        // heap usage.
        .memory_usage = [](void* context) -> uint64_t {
            auto* const memory = static_cast<NvnDirectMemory*>(context);
            const u64 nvml = memory->nvml.Get();
            return nvml != 0 ? nvml : memory->device->GetDeviceMemoryUsage();
        },
        .memory_budget = [](void* context) -> uint64_t {
            return static_cast<NvnDirectMemory*>(context)->device->GetDeviceMemoryBudget();
        },
        .memory_context = &memory_source,
        .pipeline_building = [](void* notify) {
            static_cast<VideoCore::ShaderNotify*>(notify)->MarkShaderBuilding();
        },
        .pipeline_built = [](void* notify) {
            static_cast<VideoCore::ShaderNotify*>(notify)->MarkShaderComplete();
        },
        .pipeline_context = &shader_notify,
        .async_pipelines = Settings::values.use_asynchronous_shaders.GetValue(),
        .shader_translate = &NvnDirectShaders::Translate,
        .shader_free = &NvnDirectShaders::Free,
        .shader_context = &shaders,
        .scale_up = Settings::values.resolution_info.up_scale,
        .scale_shift = Settings::values.resolution_info.down_shift,
        .debug_labels = device.HasDebuggingToolAttached() || Settings::values.renderer_debug.GetValue(),
    };
    for (size_t index = 0; index < queues.size(); ++index) {
        adopted.queues[index] = queues[(index + 1) % queues.size()];
    }
    // The replay keeps its pipeline cache, prewarm list, kept translations and the view formats
    // it learns in DM_TEMP (Banana Native's cache directory variable): unless it is set, Lichii's
    // cache directory, one folder per title named as the shader cache's (nvn_direct/<16 hex
    // digits of the program id>/), never the working directory or HOME, its fallback.
    if (const char* temp = std::getenv("DM_TEMP"); temp == nullptr || *temp == '\0') {
        const u64 title = Core::NvnHook::DirectProgramId();
        auto dir = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "nvn_direct";
        if (title != 0) {
            dir /= fmt::format("{:016x}", title);
        }
        if (Common::FS::CreateDirs(dir)) {
            const auto cache_dir = Common::FS::PathToUTF8String(dir);
#ifdef _WIN32
            memory_source.owns_cache_dir = _putenv_s("DM_TEMP", cache_dir.c_str()) == 0;
#else
            memory_source.owns_cache_dir = setenv("DM_TEMP", cache_dir.c_str(), 1) == 0;
#endif
        }
    }
    const bool up = nvn_direct_adopt_device(&adopted) != 0;
    LOG_INFO(Render_Vulkan, "NVN2 direct mode: the replay {} on Lichii's device ({}, {} queues)",
             up ? "is up" : "failed to start", device.GetModelName(), queues.size());
    return up;
}
#endif

} // Anonymous namespace

Device CreateDevice(const vk::Instance& instance, const vk::InstanceDispatch& dld, VkSurfaceKHR surface) {
    const std::vector<VkPhysicalDevice> devices = instance.EnumeratePhysicalDevices();
    if (devices.empty()) {
        LOG_ERROR(Render_Vulkan, "No Vulkan device found");
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    const u32 saved_index = Settings::values.vulkan_device.GetValue();
    const std::string saved_name = Settings::values.vulkan_device_name.GetValue();
    const auto summaries = SummarizePhysicalDevices(dld, devices);
    const PhysicalDevicePick pick = PickPhysicalDevice(summaries, saved_index, saved_name);
    const size_t device_index = pick.index;
    {
        std::string list;
        for (size_t i = 0; i < summaries.size(); ++i) {
            const char* type =
                summaries[i].type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? "discrete"
                : summaries[i].type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated"
                                                                              : "other";
            list += fmt::format("{}{}: {} ({}, {} MiB)", i == 0 ? "" : "; ", i, summaries[i].name,
                                type, summaries[i].local_memory >> 20);
        }
        LOG_INFO(Render_Vulkan,
                 "Graphics card: {} ({}), {}; saved index {}, saved name \"{}\"{}; devices: {}",
                 summaries[device_index].name, device_index, pick.reason, saved_index, saved_name,
                 Settings::values.vulkan_device.UsingGlobal() ? "" : " (game setting)", list);
    }
    const vk::PhysicalDevice physical_device(devices[device_index], dld);
    return Device(*instance, physical_device, surface, dld);
}

RendererVulkan::RendererVulkan(Core::Frontend::EmuWindow& emu_window,
                               Tegra::MaxwellDeviceMemoryManager& device_memory_,
                               Tegra::GPU& gpu_,
                               std::unique_ptr<Core::Frontend::GraphicsContext> context_)
try
    : RendererBase(emu_window, std::move(context_))
    , device_memory(device_memory_)
    , gpu(gpu_)
    , library(OpenLibrary(context.get()))
    , dld()
#ifdef EGLANTINE_STREAMLINE
    , streamline_started(Streamline::Start())
#endif
    // Create raw Vulkan instance first
    , instance(CreateInstance(*library,
                            dld,
                            VK_API_VERSION_1_1,
                            render_window.GetWindowInfo().type,
                            Settings::values.renderer_debug.GetValue()))
    // Create debug messenger if debug is enabled
    , debug_messenger(Settings::values.renderer_debug ? CreateDebugUtilsCallback(instance)
                                                    : vk::DebugUtilsMessenger{})
    , va_report_messenger(VaReport::CreateMessenger(instance))
    // Create surface
    , surface(CreateSurface(instance, render_window.GetWindowInfo()))
    , device(CreateDevice(instance, dld, *surface))
    , memory_allocator(device)
    , state_tracker()
    , scheduler(device, state_tracker)
    , swapchain(*surface,
                device,
                scheduler,
               render_window.GetFramebufferLayout().width,
               render_window.GetFramebufferLayout().height)
    , present_manager(instance,
                      render_window,
                      device,
                      memory_allocator,
                      scheduler,
                      swapchain,
                      surface)
    , blit_swapchain(device_memory,
                   device,
                   memory_allocator,
                   present_manager,
                   scheduler,
                   PresentFiltersForDisplay)
    , blit_capture(device_memory,
                   device,
                   memory_allocator,
                   present_manager,
                   scheduler,
                   PresentFiltersForDisplay)
    , blit_applet(device_memory,
                  device,
                  memory_allocator,
                  present_manager,
                  scheduler,
                  PresentFiltersForAppletCapture)
    , rasterizer(render_window, gpu, device_memory, device, memory_allocator, state_tracker, scheduler)
{

    // Force maximum clocks (off by default): a second device keeps the GPU busy. Its device
    // failing to start skips it instead of failing the whole renderer ("Vulkan initialization
    // error" and no boot).
    if (!Settings::values.renderer_force_max_clock.GetValue()) {
        LOG_INFO(Render_Vulkan, "Force maximum clocks: off");
    } else if (!device.ShouldBoostClocks()) {
        LOG_INFO(Render_Vulkan, "Force maximum clocks: skipped (driver not on the list, Steam "
                                "Deck, or a debugging tool attached)");
    } else {
        try {
            turbo_mode.emplace(instance, dld);
            scheduler.RegisterOnSubmit([this] { turbo_mode->QueueSubmitted(); });
            LOG_INFO(Render_Vulkan, "Force maximum clocks: running (costs power; not for laptops "
                                    "on battery)");
        } catch (const std::exception& exception) {
            turbo_mode.reset();
            LOG_WARNING(Render_Vulkan, "Force maximum clocks: skipped (its device failed: {})",
                        exception.what());
        }
    }

#ifdef HAS_RESHADE
    VideoCore::FxChain::Instance().LoadFromSettings();
#endif

    Report();
#ifdef LICHII_NVN_DIRECT
    if (const char* trace = std::getenv("EDEN_NX2_VRAM_TRACE");
        trace != nullptr && std::strcmp(trace, "1") == 0) {
        vram_trace = std::jthread([this](std::stop_token stop) {
            NvmlProcessMemory nvml{device};
            while (!stop.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::seconds{1});
                const u64 process = nvml.Get();
                const u64 slack = device.GetDeviceMemorySlack();
                const u64 heaps = device.GetDeviceMemoryUsage() + slack;
                LOG_INFO(Render_Vulkan,
                         "VRAM trace: NVML {} MiB, heap usage {} MiB (VMA unused {} MiB), "
                         "outside the heaps {} MiB",
                         process >> 20, heaps >> 20, slack >> 20,
                         (static_cast<s64>(process) - static_cast<s64>(heaps)) >> 20);
            }
        });
    }
    if (device.IsNvnDirect()) {
        Core::NvnHook::TraceStartup("before replay setup");
        if (!device.IsOptimalAstcSupported() && device.IsKhrPushDescriptorSupported()) {
            nvn_direct_astc.emplace(device, memory_allocator);
        }
        const PipelineCache& pipeline_cache = rasterizer.GetPipelineCache();
        nvn_direct_shaders.emplace(device, pipeline_cache.GetProfile(), pipeline_cache.GetHostInfo());
        nvn_direct_memory.reset(new NvnDirectMemory{&device, NvmlProcessMemory{device}});
        if (!AdoptNvnDirect(dld, *instance, device, scheduler,
                            nvn_direct_astc ? &*nvn_direct_astc : nullptr, *nvn_direct_shaders,
                            *nvn_direct_memory, gpu_.ShaderNotify())) {
            throw std::runtime_error{"NVN direct replay rejected the Vulkan device"};
        }
        Core::NvnHook::DirectStarted();
    }
#endif
} catch (const vk::Exception& exception) {
    LOG_ERROR(Render_Vulkan, "Vulkan initialization failed with error: {}", exception.what());
    throw std::runtime_error{fmt::format("Vulkan initialization error {}", exception.what())};
}

RendererVulkan::~RendererVulkan() {
#ifdef LICHII_NVN_DIRECT
    // NVN2 direct mode: the replay's threads leave the device (and the ASTC
    // decode it calls back into) before both go away, and its objects go once the device is idle.
    if (device.IsNvnDirect()) {
        nvn_direct_park();
        scheduler.WaitWorker();
        present_manager.WaitPresent();
        scheduler.RegisterOnSubmit([] {});
        const auto result = device.GetLogical().WaitIdle();
        if (result == VK_ERROR_DEVICE_LOST) {
            device.ReportLoss();
        }
        if (result != VK_SUCCESS) {
            LOG_CRITICAL(Render_Vulkan, "NVN direct: the device did not reach idle ({})",
                         static_cast<int>(result));

            std::_Exit(EXIT_FAILURE);
        }
        nvn_direct_destroy();
        return;
    }
#endif
    scheduler.RegisterOnSubmit([] {});
    void(device.GetLogical().WaitIdle());
}

void RendererVulkan::Composite(std::span<const Tegra::FramebufferConfig> framebuffers) {
#ifdef LICHII_NVN_DIRECT
    nvn_direct_frame replay_frame{};
    if (device.IsNvnDirect()) {
        if (framebuffers.size() != 1) {
            return;
        }
        const auto address = framebuffers.front().address + framebuffers.front().offset;
        if ((address & NVN_DIRECT_FRAME_TAG) == 0 ||
            !nvn_direct_present_frame(address & ~NVN_DIRECT_FRAME_TAG, &replay_frame)) {
            return;
        }
        rasterizer.SetDirectDisplayFrame(&replay_frame);
    }
    SCOPE_EXIT {
        if (device.IsNvnDirect()) {
            rasterizer.SetDirectDisplayFrame(nullptr);
        }
    };
#endif
    const bool trace = Common::DkbFrameTrace::Enabled();
    const auto entered = trace ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
    SCOPE_EXIT {
        render_window.OnFrameDisplayed();
    };

    // One follower: the frame fence's syncpoint can run this on E while R still plays the frame.
    // The scheduler is R's: wait for R before the first record (before 2026-11-18 a lend inside
    // AccelerateDisplay did that by accident; FRITCHII_SLOW_GPU's present-pass records ran before
    // it and raced R's DispatchWork, a null chunk on GPUBack; helper EC).
    if (VideoCommon::OneFollower::OnEngineThread()) {
        VideoCommon::OneFollower::Drain();
    }

    RenderAppletCaptureLayer(framebuffers);

    if (!render_window.IsShown()) {
        return;
    }

    RenderScreenshot(framebuffers);
    const auto acquiring = trace ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
    Frame* frame = present_manager.GetRenderFrame();
    const auto acquired = trace ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};

    scheduler.RequestOutsideRenderPassOperationContext();
    blit_swapchain.DrawToFrame(device, rasterizer, frame, framebuffers,
                               render_window.GetFramebufferLayout(),
                               present_manager.SwapchainImageCount(),
                               swapchain.GetImageViewFormat());

    if (trace) {
        scheduler.MarkFrameEnd(++trace_frame);
    }
    frame->render_tick = scheduler.Flush(*frame->render_ready);

    const auto presenting = trace ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{};
    present_manager.Present(frame);
    const auto presented = trace ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
    // Hand the work recorded so far to the worker right after the present.
    scheduler.DispatchWork();

    gpu.RendererFrameEndNotify();
    rasterizer.TickFrame();
    if (trace) {
        const auto end = std::chrono::steady_clock::now();
        const auto ms = [](auto duration) {
            return std::chrono::duration<double, std::milli>(duration).count();
        };
        if (trace_cpu_rate.Ready(end)) {
            LOG_INFO(Render_Vulkan,
                     "DKB renderer renderer_frame={} host_pacing_mode={} vsync_mode={} "
                     "cpu_frame_wall_ms={:.3f} "
                     "cpu_composite_wall_ms={:.3f} capture_wall_ms={:.3f} "
                     "acquire_frame_wall_ms={:.3f} record_submit_wall_ms={:.3f} "
                     "present_call_wall_ms={:.3f} frame_end_wall_ms={:.3f}",
                     trace_frame, static_cast<int>(Settings::values.frame_pacing_mode.GetValue()),
                     static_cast<int>(Settings::values.vsync_mode.GetValue()),
                     trace_last_frame == std::chrono::steady_clock::time_point{}
                         ? -1.0 : ms(entered - trace_last_frame),
                     ms(end - entered), ms(acquiring - entered), ms(acquired - acquiring),
                     ms(presenting - acquired), ms(presented - presenting), ms(end - presented));
        }
        trace_last_frame = entered;
    }
}

void RendererVulkan::Report() const {
    using namespace Common::Literals;
    const std::string vendor_name{device.GetVendorName()};
    const std::string model_name{device.GetModelName()};
    const std::string driver_version = GetDriverVersion(device);
    const std::string driver_name = fmt::format("{} {}", vendor_name, driver_version);

    const std::string api_version = GetReadableVersion(device.ApiVersion());

    const std::string extensions = BuildCommaSeparatedExtensions(device.GetAvailableExtensions());

    const auto available_vram = static_cast<f64>(device.GetDeviceLocalMemory()) / f64{1_GiB};

    LOG_INFO(Render_Vulkan, "Driver: {}", driver_name);
    LOG_INFO(Render_Vulkan, "Device: {}", model_name);
    LOG_INFO(Render_Vulkan, "Vulkan: {}", api_version);
    // The caches' memory limit, not the card's size: with Conservative VRAM use it is capped at
    // 6 GiB + 1 GiB per resolution scale step, so a 16 GB card reads 7.00 GiB at 1x.
    std::string vram_limit;
    if (device.IsIntegrated()) {
        vram_limit = "integrated GPU: a share of system memory";
    } else if (Settings::values.vram_usage_mode.GetValue() ==
               Settings::VramUsageMode::Aggressive) {
        vram_limit = "Aggressive: the driver's budget less up to 1 GiB";
    } else {
        vram_limit = fmt::format("Conservative cap: 6 GiB + 1 GiB x {}",
                                 Settings::values.resolution_info.ScaleUp(1));
    }
    u64 main_usage = 0;
    u64 main_budget = 0;
    const std::string main_heap =
        device.MainHeapUsage(main_usage, main_budget)
            ? fmt::format("main heap {}: budget {:.2f} GiB, usage {:.2f} GiB",
                          device.GetMainHeap(), static_cast<f64>(main_budget) / f64{1_GiB},
                          static_cast<f64>(main_usage) / f64{1_GiB})
            : fmt::format("main heap {}", device.GetMainHeap());
    LOG_INFO(Render_Vulkan, "Available VRAM: {:.2f} GiB (memory limit for the caches; {}; {})",
             available_vram, vram_limit, main_heap);
    device.LogMemoryHeaps();
    if (VideoCommon::RenderTrace::Enabled()) {
        LOG_INFO(Render_Vulkan, "Render trace enabled: interval=600 frames, log_filter={}",
                 Settings::values.log_filter.GetValue());
    }
    LOG_DEBUG(Render_Vulkan,
              "Render trace capabilities: geometry_shader={} transform_feedback={} "
              "geometry_streams={} mesh_shader={}",
              device.GetPhysical().GetFeatures().geometryShader, device.IsExtTransformFeedbackSupported(),
              device.AreTransformFeedbackGeometryStreamsSupported(), device.IsExtMeshShaderSupported());
}

vk::Buffer RendererVulkan::RenderToBuffer(std::span<const Tegra::FramebufferConfig> framebuffers,
                                          const Layout::FramebufferLayout& layout, VkFormat format,
                                          VkDeviceSize buffer_size) {
    auto frame = [&]() {
        Frame f{};
        f.image =
            CreateWrappedImage(memory_allocator, VkExtent2D{layout.width, layout.height}, format);
        f.image_view = CreateWrappedImageView(device, f.image, format);
        f.framebuffer = blit_capture.CreateFramebuffer(device, layout, *f.image_view, format);
        return f;
    }();

    auto dst_buffer = CreateWrappedBuffer(memory_allocator, buffer_size, MemoryUsage::Download);
    blit_capture.DrawToFrame(device, rasterizer, &frame, framebuffers, layout, 1, format);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        DownloadColorImage(cmdbuf, *frame.image, *dst_buffer,
                           VkExtent3D{layout.width, layout.height, 1});
    });

    // Ensure the copy is fully completed before saving the capture
    scheduler.Finish();

    // Copy backing image data to the capture buffer
    dst_buffer.Invalidate();
    return dst_buffer;
}

void RendererVulkan::RenderScreenshot(std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (!renderer_settings.screenshot_requested) {
        return;
    }

    const auto screenshot_layers = Tegra::FilterLayerStack(
        framebuffers, renderer_settings.screenshot_layer_stack, screenshot_layer_scratch);

    const auto& layout{renderer_settings.screenshot_framebuffer_layout};
    const auto dst_buffer = RenderToBuffer(screenshot_layers, layout, VK_FORMAT_B8G8R8A8_UNORM,
                                           layout.width * layout.height * 4);

    std::memcpy(renderer_settings.screenshot_bits, dst_buffer.Mapped().data(),
                dst_buffer.Mapped().size());
    renderer_settings.screenshot_complete_callback(false);
    renderer_settings.screenshot_requested = false;
}

std::vector<u8> RendererVulkan::GetAppletCaptureBuffer() {
    using namespace VideoCore::Capture;

    // Same as Composite: the scheduler is R's (this Finish ran on E too).
    if (VideoCommon::OneFollower::OnEngineThread()) {
        VideoCommon::OneFollower::Drain();
    }

    std::vector<u8> out(VideoCore::Capture::TiledSize);

    if (!applet_frame.image) {
        return out;
    }

    const auto dst_buffer =
        CreateWrappedBuffer(memory_allocator, VideoCore::Capture::TiledSize, MemoryUsage::Download);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        DownloadColorImage(cmdbuf, *applet_frame.image, *dst_buffer, CaptureImageExtent);
    });

    // Ensure the copy is fully completed before writing the capture
    scheduler.Finish();

    // Swizzle image data to the capture buffer
    dst_buffer.Invalidate();
    Tegra::Texture::SwizzleTexture(out, dst_buffer.Mapped(), BytesPerPixel, LinearWidth,
                                   LinearHeight, LinearDepth, BlockHeight, BlockDepth);

    return out;
}

void RendererVulkan::RenderAppletCaptureLayer(
    std::span<const Tegra::FramebufferConfig> framebuffers) {
    const auto capture_layers = Tegra::FilterLayerStack(
        framebuffers, Service::Nvnflinger::LayerStackId::LastFrame, applet_capture_layers);

    if (capture_layers.empty())
        return;

    if (!applet_frame.image) {
        applet_frame.image = CreateWrappedImage(memory_allocator, CaptureImageSize, CaptureFormat);
        applet_frame.image_view = CreateWrappedImageView(device, applet_frame.image, CaptureFormat);
        applet_frame.framebuffer = blit_applet.CreateFramebuffer(device,
            VideoCore::Capture::Layout, *applet_frame.image_view, CaptureFormat);
    }

    scheduler.RequestOutsideRenderPassOperationContext();
    blit_applet.DrawToFrame(device, rasterizer, &applet_frame, capture_layers, VideoCore::Capture::Layout, 1,
                            CaptureFormat);
}

#ifdef EGLANTINE_STREAMLINE
VideoCore::DlssStatus RendererVulkan::GetDlssStatus() const {
    return StreamlineDlssUpscaler::Status();
}
#endif

} // namespace Vulkan
