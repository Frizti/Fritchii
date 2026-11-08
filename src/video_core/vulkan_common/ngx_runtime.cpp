// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/vulkan_common/ngx.h"

#include <cstdlib>
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan::Ngx {
std::filesystem::path RuntimeDirectory() {
    if (const char* path = std::getenv("EDEN_NGX_DIR"); path && *path) return path;
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path{} : executable.parent_path() / "ngx";
}
bool Start() {
    if (!Settings::values.nvidia_dlss.GetValue()) return false;
    const bool ready = Prepare(RuntimeDirectory(),
        Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "ngx");
    if (!ready) LOG_WARNING(Render_Vulkan, "NVIDIA DLSS (native NGX): runtime missing from {}; keeping fallback",
                            RuntimeDirectory().string());
    return ready;
}
bool AttachDevice(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                  u32, vk::DeviceDispatch& dispatch) {
    const bool ready = Initialize(instance, physical, device, dispatch.vkGetInstanceProcAddr,
                                  dispatch.vkGetDeviceProcAddr);
    LOG_INFO(Render_Vulkan, "NVIDIA DLSS (native NGX): device {} (result {:#x})",
             ready ? "ready" : "unavailable", LastResult());
    return ready;
}
} // namespace Vulkan::Ngx
