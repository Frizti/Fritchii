// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/vulkan_common/ngx.h"

#include <cstdlib>
#include <optional>
#include <string_view>
#include <nvsdk_ngx_helpers_vk.h>

namespace Vulkan::Ngx {
namespace {
constexpr const char* PROJECT_ID = "8d3e2f9a-6b41-4c7e-9a52-1f0c3b7d5e64";
enum class Phase { Disabled, Prepared, Attached };
struct Runtime {
    Phase phase{};
    VkDevice device{};
    std::wstring path;
    std::wstring cache;
    const wchar_t* search_path{};
    NVSDK_NGX_FeatureCommonInfo info{};
    NVSDK_NGX_Result result{};
};
Runtime runtime;

NVSDK_NGX_FeatureDiscoveryInfo Discovery() {
    NVSDK_NGX_FeatureDiscoveryInfo discovery{};
    discovery.SDKVersion = NVSDK_NGX_Version_API;
    discovery.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    discovery.Identifier.v.ProjectDesc.ProjectId = PROJECT_ID;
    discovery.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    discovery.Identifier.v.ProjectDesc.EngineVersion = "Fritchii-v4";
    discovery.ApplicationDataPath = runtime.cache.c_str();
    discovery.FeatureInfo = &runtime.info;
    return discovery;
}

std::vector<std::string> Names(NVSDK_NGX_Result result, u32 count,
                               const VkExtensionProperties* properties) {
    runtime.result = result;
    std::vector<std::string> names;
    if (NVSDK_NGX_FAILED(result)) {
        runtime.phase = Phase::Disabled;
        return names;
    }
    for (u32 i = 0; i < count; ++i) {
        names.emplace_back(properties[i].extensionName);
    }
    return names;
}

bool EnvEnabled(const char* name) {
    const char* value = std::getenv(name);
    return !value || std::atoi(value) != 0;
}

struct Configuration {
    u32 width, height, out_width, out_height;
    int flags;
    NVSDK_NGX_PerfQuality_Value quality;
    u32 preset;
    bool operator==(const Configuration&) const = default;
};
Configuration Config(const NvidiaDlssInputs& in) {
    const float ratio = static_cast<float>(in.render_size.width) / static_cast<float>(in.upscale_size.width);
    const auto quality = ratio >= 0.99f ? NVSDK_NGX_PerfQuality_Value_DLAA
                       : ratio >= 0.65f ? NVSDK_NGX_PerfQuality_Value_MaxQuality
                       : ratio >= 0.57f ? NVSDK_NGX_PerfQuality_Value_Balanced
                       : ratio >= 0.49f ? NVSDK_NGX_PerfQuality_Value_MaxPerf
                                        : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (in.depth_inverted) flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (in.hdr && EnvEnabled("EDEN_NX2_DLSS_HDR")) flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    if (in.hdr && EnvEnabled("EDEN_NX2_DLSS_AUTO_EXPOSURE"))
        flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    u32 preset = NVSDK_NGX_DLSS_Hint_Render_Preset_K;
    if (const char* value = std::getenv("EDEN_NX2_DLSS_PRESET")) {
        if (value[0] >= 'J' && value[0] <= 'M') {
            preset = NVSDK_NGX_DLSS_Hint_Render_Preset_J + static_cast<u32>(value[0] - 'J');
        } else if (std::string_view{value} == "default") {
            preset = NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
        }
    }
    return {in.render_size.width, in.render_size.height, in.upscale_size.width,
            in.upscale_size.height, flags, quality, preset};
}

NVSDK_NGX_Resource_VK Resource(const NvidiaDlssImage& image, VkExtent2D extent, bool write) {
    return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, image.range,
                                                image.format, extent.width, extent.height, write);
}
} // namespace

bool Prepare(const std::filesystem::path& path, const std::filesystem::path& cache) {
    if (runtime.phase == Phase::Attached) return true;
    runtime.phase = Phase::Disabled;
    std::error_code error;
    if (path.empty() || !std::filesystem::is_regular_file(path / "libnvidia-ngx-dlss.so", error)) return false;
    std::filesystem::create_directories(cache, error);
    if (error) return false;
    runtime.path = std::filesystem::absolute(path).wstring();
    runtime.cache = std::filesystem::absolute(cache).wstring();
    runtime.search_path = runtime.path.c_str();
    runtime.info = {};
    runtime.info.PathListInfo = {&runtime.search_path, 1};
    runtime.phase = Phase::Prepared;
    return true;
}

bool WantsDevice() { return runtime.phase == Phase::Prepared; }
bool IsDlssAvailable() { return runtime.phase == Phase::Attached; }
u32 LastResult() { return static_cast<u32>(runtime.result); }

std::vector<std::string> InstanceExtensions() {
    if (!WantsDevice()) return {};
    const auto discovery = Discovery();
    u32 count{};
    VkExtensionProperties* properties{};
    const auto result = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
        &discovery, &count, &properties);
    return Names(result, count, properties);
}
std::vector<std::string> DeviceExtensions(VkInstance instance, VkPhysicalDevice physical) {
    if (!WantsDevice()) return {};
    const auto discovery = Discovery();
    u32 count{};
    VkExtensionProperties* properties{};
    const auto result = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
        instance, physical, &discovery, &count, &properties);
    return Names(result, count, properties);
}

bool Initialize(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                PFN_vkGetInstanceProcAddr get_instance, PFN_vkGetDeviceProcAddr get_device) {
    if (!WantsDevice()) return false;
    runtime.result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "Fritchii-v4", runtime.cache.c_str(),
        instance, physical, device, get_instance, get_device, &runtime.info);
    if (NVSDK_NGX_FAILED(runtime.result)) {
        runtime.phase = Phase::Disabled;
        return false;
    }
    runtime.device = device;
    runtime.phase = Phase::Attached;
    NVSDK_NGX_Parameter* capabilities{};
    runtime.result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&capabilities);
    int available{};
    if (NVSDK_NGX_SUCCEED(runtime.result) && capabilities) {
        runtime.result = capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        NVSDK_NGX_VULKAN_DestroyParameters(capabilities);
    }
    if (NVSDK_NGX_FAILED(runtime.result) || !available) {
        Stop();
        return false;
    }
    return true;
}

void Stop() {
    if (runtime.phase == Phase::Attached) NVSDK_NGX_VULKAN_Shutdown1(runtime.device);
    runtime.phase = Phase::Disabled;
    runtime.device = VK_NULL_HANDLE;
}

struct Feature::State {
    NVSDK_NGX_Handle* handle{};
    NVSDK_NGX_Parameter* parameters{};
    std::optional<Configuration> config;
    NVSDK_NGX_Result result{};
    void Release() {
        if (handle) NVSDK_NGX_VULKAN_ReleaseFeature(handle);
        if (parameters) NVSDK_NGX_VULKAN_DestroyParameters(parameters);
        handle = nullptr;
        parameters = nullptr;
        config.reset();
    }
};
Feature::Feature() : state{std::make_unique<State>()} {}
Feature::~Feature() { state->Release(); }
u32 Feature::LastResult() const { return static_cast<u32>(state->result); }
bool Feature::NeedsRecreate(const NvidiaDlssInputs& inputs) const {
    return !state->config || *state->config != Config(inputs);
}

bool Feature::Evaluate(VkCommandBuffer command, const NvidiaDlssInputs& in,
                       const NvidiaDlssImage& output) {
    if (!IsDlssAvailable() || !in.render_size.width || !in.render_size.height ||
        !in.upscale_size.width || !in.upscale_size.height) return false;
    const Configuration config = Config(in);
    const bool recreate = NeedsRecreate(in);
    if (recreate) {
        state->Release();
        state->result = NVSDK_NGX_VULKAN_AllocateParameters(&state->parameters);
        if (NVSDK_NGX_FAILED(state->result)) return false;
        for (const char* key : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                               NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                               NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                               NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                               NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                               NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality}) {
            state->parameters->Set(key, config.preset);
        }
        NVSDK_NGX_DLSS_Create_Params create{};
        create.Feature = {config.width, config.height, config.out_width, config.out_height,
                          config.quality};
        create.InFeatureCreateFlags = config.flags;
        state->result = NGX_VULKAN_CREATE_DLSS_EXT1(runtime.device, command, 1, 1,
                                                   &state->handle, state->parameters, &create);
        if (NVSDK_NGX_FAILED(state->result)) return false;
        state->config = config;
    }
    auto color = Resource(in.color, in.render_size, false);
    auto depth = Resource(in.depth, in.render_size, false);
    auto motion = Resource(in.motion_vectors, in.render_size, false);
    auto target = Resource(output, in.upscale_size, true);
    NVSDK_NGX_VK_DLSS_Eval_Params evaluate{};
    evaluate.Feature.pInColor = &color;
    evaluate.Feature.pInOutput = &target;
    evaluate.pInDepth = &depth;
    evaluate.pInMotionVectors = &motion;
    evaluate.InJitterOffsetX = in.jitter[0];
    evaluate.InJitterOffsetY = in.jitter[1];
    evaluate.InMVScaleX = in.motion_vector_scale[0];
    evaluate.InMVScaleY = in.motion_vector_scale[1];
    evaluate.InRenderSubrectDimensions = {config.width, config.height};
    evaluate.InReset = in.reset || recreate;
    evaluate.InPreExposure = 1.0f;
    evaluate.InExposureScale = 1.0f;
    state->result = NGX_VULKAN_EVALUATE_DLSS_EXT(command, state->handle, state->parameters, &evaluate);
    return NVSDK_NGX_SUCCEED(state->result);
}
} // namespace Vulkan::Ngx
