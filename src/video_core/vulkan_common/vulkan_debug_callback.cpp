// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include "common/fritchii_write_ring.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/vulkan_common/vulkan_debug_callback.h"
#include "video_core/gpu_logging/gpu_logging.h"

namespace Vulkan {
namespace {

// Helper to get message type as string for GPU logging
const char* GetMessageTypeName(VkDebugUtilsMessageTypeFlagsEXT type) {
    if (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) {
        return "Validation";
    } else if (type & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) {
        return "Performance";
    } else {
        return "General";
    }
}

VkBool32 DebugUtilCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                           VkDebugUtilsMessageTypeFlagsEXT type,
                           const VkDebugUtilsMessengerCallbackDataEXT* data,
                           [[maybe_unused]] void* user_data) {
    // Skip logging known false-positive validation errors
    switch (static_cast<u32>(data->messageIdNumber)) {
#ifdef __ANDROID__
    case 0xbf9cf353u: // VUID-vkCmdBindVertexBuffers2-pBuffers-04111
    // The below are due to incorrect reporting of extendedDynamicState
    case 0x1093bebbu: // VUID-vkCmdSetCullMode-None-03384
    case 0x9215850fu: // VUID-vkCmdSetDepthTestEnable-None-03352
    case 0x86bf18dcu: // VUID-vkCmdSetDepthWriteEnable-None-03354
    case 0x0792ad08u: // VUID-vkCmdSetStencilOp-None-03351
    case 0x93e1ba4eu: // VUID-vkCmdSetFrontFace-None-03383
    case 0xac9c13c5u: // VUID-vkCmdSetStencilTestEnable-None-03350
    case 0xc9a2001bu: // VUID-vkCmdSetDepthBoundsTestEnable-None-03349
    case 0x8b7159a7u: // VUID-vkCmdSetDepthCompareOp-None-03353
    // The below are due to incorrect reporting of extendedDynamicState2
    case 0xb13c8036u: // VUID-vkCmdSetDepthBiasEnable-None-04872
    case 0xdff2e5c1u: // VUID-vkCmdSetRasterizerDiscardEnable-None-04871
    case 0x0cc85f41u: // VUID-vkCmdSetPrimitiveRestartEnable-None-04866
    case 0x01257b492: // VUID-vkCmdSetLogicOpEXT-None-0486
    // The below are due to incorrect reporting of vertexInputDynamicState
    case 0x398e0dabu: // VUID-vkCmdSetVertexInputEXT-None-04790
    // The below are due to incorrect reporting of extendedDynamicState3
    case 0x970c11a5u: // VUID-vkCmdSetColorWriteMaskEXT-extendedDynamicState3ColorWriteMask-07364
    case 0x6b453f78u: // VUID-vkCmdSetColorBlendEnableEXT-extendedDynamicState3ColorBlendEnable-07355
    case 0xf66469d0u: // VUID-vkCmdSetColorBlendEquationEXT-extendedDynamicState3ColorBlendEquation-07356
    case 0x1d43405eu: // VUID-vkCmdSetLogicOpEnableEXT-extendedDynamicState3LogicOpEnable-07365
    case 0x638462e8u: // VUID-vkCmdSetDepthClampEnableEXT-extendedDynamicState3DepthClampEnable-07448
    // Misc
    case 0xe0a2da61u: // VUID-vkCmdDrawIndexed-format-07753
#else
    case 0x682a878au: // VUID-vkCmdBindVertexBuffers2EXT-pBuffers-parameter
    case 0x99fb7dfdu: // UNASSIGNED-RequiredParameter (vkCmdBindVertexBuffers2EXT pBuffers[0])
    case 0xe8616bf2u: // Bound VkDescriptorSet 0x0[] was destroyed. Likely push_descriptor related
    case 0x1608dec0u: // Image layout in vkUpdateDescriptorSet doesn't match descriptor use
    case 0x55362756u: // Descriptor binding and framebuffer attachment overlap
#endif
        return VK_FALSE;
    default:
        break;
    }
    const std::string_view message{data->pMessage};
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        LOG_CRITICAL(Render_Vulkan, "{}", message);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        LOG_WARNING(Render_Vulkan, "{}", message);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        LOG_INFO(Render_Vulkan, "{}", message);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) {
        LOG_DEBUG(Render_Vulkan, "{}", message);
    }

    // Route to GPU logger for tracking Vulkan validation messages
    if (GPU::Logging::IsActive() &&
        Settings::values.gpu_log_vulkan_calls.GetValue()) {
        // Convert severity to result code for logging (negative = error)
        int result_code = 0;
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
            result_code = -1;
        } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
            result_code = -2;
        }

        // Get message ID name or use generic name
        const char* call_name = data->pMessageIdName ? data->pMessageIdName : "VulkanDebug";

        GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
            call_name,
            std::string(GetMessageTypeName(type)) + ": " + std::string(message),
            result_code
        );
    }

    return VK_FALSE;
}

} // Anonymous namespace

vk::DebugUtilsMessenger CreateDebugUtilsCallback(const vk::Instance& instance) {
    return instance.CreateDebugUtilsMessenger(VkDebugUtilsMessengerCreateInfoEXT{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext = nullptr,
        .flags = 0,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = DebugUtilCallback,
        .pUserData = nullptr,
    });
}

namespace VaReport {
namespace {

/// One bind or unbind the driver reported.
struct Event {
    u64 address;
    u64 size;
    u64 handle;
    u64 nanoseconds; ///< steady clock
    u64 sequence;    ///< 1-based event number
    std::array<u64, 2> thread; ///< the reporting thread's name (Common::WriteRing's 16 chars)
    VkObjectType object_type;
    bool unbind;
    bool failed;   ///< reported at warning or error severity (a failed binding)
    bool internal; ///< VK_DEVICE_ADDRESS_BINDING_INTERNAL_OBJECT_BIT_EXT: the driver's own
};

constexpr size_t RingSize = 1 << 16;

struct State {
    std::mutex mutex;
    std::unique_ptr<std::array<Event, RingSize>> ring;
    u64 events = 0;
    u64 binds = 0;
    u64 unbinds = 0;
};
State& Get() {
    static State state;
    return state;
}

std::atomic<bool> g_instance_ready{};
std::atomic<bool> g_device_ready{};

u64 NowNanoseconds() noexcept {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

const char* ObjectTypeName(VkObjectType type) noexcept {
    switch (type) {
    case VK_OBJECT_TYPE_BUFFER:
        return "buffer";
    case VK_OBJECT_TYPE_IMAGE:
        return "image";
    case VK_OBJECT_TYPE_DEVICE_MEMORY:
        return "device memory";
    case VK_OBJECT_TYPE_COMMAND_BUFFER:
        return "command buffer";
    case VK_OBJECT_TYPE_COMMAND_POOL:
        return "command pool";
    case VK_OBJECT_TYPE_DESCRIPTOR_POOL:
        return "descriptor pool";
    case VK_OBJECT_TYPE_DESCRIPTOR_SET:
        return "descriptor set";
    case VK_OBJECT_TYPE_PIPELINE:
        return "pipeline";
    case VK_OBJECT_TYPE_SHADER_MODULE:
        return "shader module";
    case VK_OBJECT_TYPE_QUERY_POOL:
        return "query pool";
    case VK_OBJECT_TYPE_QUEUE:
        return "queue";
    case VK_OBJECT_TYPE_DEVICE:
        return "device";
    case VK_OBJECT_TYPE_UNKNOWN:
        return "unknown (driver internal)";
    default:
        return "other";
    }
}

VkBool32 Callback([[maybe_unused]] VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                  VkDebugUtilsMessageTypeFlagsEXT type,
                  const VkDebugUtilsMessengerCallbackDataEXT* data,
                  [[maybe_unused]] void* user_data) {
    if ((type & VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT) == 0 ||
        data == nullptr) {
        return VK_FALSE;
    }
    const VkDeviceAddressBindingCallbackDataEXT* binding = nullptr;
    for (auto* next = static_cast<const VkBaseInStructure*>(data->pNext); next != nullptr;
         next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT) {
            binding = reinterpret_cast<const VkDeviceAddressBindingCallbackDataEXT*>(next);
            break;
        }
    }
    if (binding == nullptr) {
        return VK_FALSE;
    }
    const bool has_object = data->objectCount > 0 && data->pObjects != nullptr;
    Event event{
        .address = binding->baseAddress,
        .size = binding->size,
        .handle = has_object ? data->pObjects[0].objectHandle : 0,
        .nanoseconds = NowNanoseconds(),
        .sequence = 0,
        .thread = Common::WriteRing::t_thread,
        .object_type = has_object ? data->pObjects[0].objectType : VK_OBJECT_TYPE_UNKNOWN,
        .unbind = binding->bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT,
        .failed = (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) != 0,
        .internal = (binding->flags & VK_DEVICE_ADDRESS_BINDING_INTERNAL_OBJECT_BIT_EXT) != 0,
    };
    State& state = Get();
    std::scoped_lock lock{state.mutex};
    if (!state.ring) {
        state.ring = std::make_unique<std::array<Event, RingSize>>();
    }
    event.sequence = ++state.events;
    (event.unbind ? state.unbinds : state.binds) += 1;
    (*state.ring)[(event.sequence - 1) % RingSize] = event;
    return VK_FALSE;
}

std::string ThreadName(const std::array<u64, 2>& words) {
    std::array<char, 17> chars{};
    std::memcpy(chars.data(), words.data(), 16);
    return chars[0] != '\0' ? std::string{chars.data()} : std::string{"unnamed"};
}

} // Anonymous namespace

bool Requested() noexcept {
    static const bool on = [] {
        const char* const value = std::getenv("FRITCHII_VA_REPORT");
        return value != nullptr && value[0] == '1';
    }();
    return on;
}

void NoteInstanceReady() noexcept {
    g_instance_ready.store(true, std::memory_order_relaxed);
}

bool InstanceReady() noexcept {
    return g_instance_ready.load(std::memory_order_relaxed);
}

void NoteDeviceReady() noexcept {
    g_device_ready.store(true, std::memory_order_relaxed);
}

vk::DebugUtilsMessenger CreateMessenger(const vk::Instance& instance) {
    if (!Requested()) {
        return vk::DebugUtilsMessenger{};
    }
    if (!InstanceReady()) {
        LOG_WARNING(Render_Vulkan, "GPU address report (FRITCHII_VA_REPORT=1): off, the Vulkan "
                                   "loader has no VK_EXT_debug_utils");
        return vk::DebugUtilsMessenger{};
    }
    LOG_INFO(Render_Vulkan, "GPU address report (FRITCHII_VA_REPORT=1): messenger on, the last {} "
                            "binds and unbinds are kept for a device loss",
             RingSize);
    return instance.CreateDebugUtilsMessenger(VkDebugUtilsMessengerCreateInfoEXT{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext = nullptr,
        .flags = 0,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT,
        .pfnUserCallback = Callback,
        .pUserData = nullptr,
    });
}

void LogOwners(u64 address, u64 precision) noexcept {
    if (!Requested()) {
        return;
    }
    if (!g_device_ready.load(std::memory_order_relaxed)) {
        LOG_CRITICAL(Render_Vulkan, "GPU address report: off on this device (the driver has no "
                                    "VK_EXT_device_address_binding_report, or no debug utils)");
        return;
    }
    precision = (std::max)(precision, u64{1});
    const u64 lower = address & ~(precision - 1);
    const u64 upper = lower + precision;
    constexpr u64 NearBytes = 4ULL << 20; // ranges within 4 MiB, when none holds the page
    State& state = Get();
    // The process ends after the report: never wait long on a messenger stuck mid-event.
    std::unique_lock lock{state.mutex, std::defer_lock};
    for (int attempt = 0; attempt < 200 && !lock.try_lock(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    if (!lock.owns_lock() || !state.ring) {
        LOG_CRITICAL(Render_Vulkan, "GPU address report: no events recorded ({})",
                     lock.owns_lock() ? "empty" : "busy");
        return;
    }
    const u64 now = NowNanoseconds();
    const u64 count = (std::min<u64>)(state.events, RingSize);
    LOG_CRITICAL(Render_Vulkan,
                 "GPU address report for [{:#x}, {:#x}): {} events this session ({} binds, {} "
                 "unbinds), the last {} kept; holders newest first:",
                 lower, upper, state.events, state.binds, state.unbinds, count);
    const auto describe = [&](const Event& event, const char* relation) {
        LOG_CRITICAL(Render_Vulkan,
                     "GPU address report: #{} {} {}{}{} handle {:#x}, [{:#x}, {:#x}) {} KiB, {:.1f} ms "
                     "before the report, thread {} ({})",
                     event.sequence, event.unbind ? "UNBIND" : "bind",
                     ObjectTypeName(event.object_type), event.internal ? " (driver internal)" : "",
                     event.failed ? " (failed)" : "", event.handle, event.address, event.address + event.size,
                     event.size >> 10, static_cast<double>(now - event.nanoseconds) / 1e6,
                     ThreadName(event.thread), relation);
    };
    u32 holders = 0;
    for (u64 i = 0; i < count && holders < 16; ++i) {
        const Event& event = (*state.ring)[(state.events - 1 - i) % RingSize];
        if (event.address < upper && lower < event.address + event.size) {
            describe(event, "holds the page");
            ++holders;
        }
    }
    if (holders != 0) {
        return;
    }
    LOG_CRITICAL(Render_Vulkan, "GPU address report: no range in the ring holds the page (older "
                                "than the ring, or never reported); nearest within 4 MiB:");
    u32 nearby = 0;
    for (u64 i = 0; i < count && nearby < 8; ++i) {
        const Event& event = (*state.ring)[(state.events - 1 - i) % RingSize];
        const u64 end = event.address + event.size;
        const bool before = end <= lower && lower - end <= NearBytes;
        const bool after = event.address >= upper && event.address - upper <= NearBytes;
        if (before || after) {
            describe(event, before ? "ends below the page" : "starts above the page");
            ++nearby;
        }
    }
}

} // namespace VaReport

} // namespace Vulkan
