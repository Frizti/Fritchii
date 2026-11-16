// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Streamline's API as loaded from the user's sl.interposer.dll. Only for the
// Streamline sources: it pulls in <windows.h> and Streamline's (MIT-licensed) headers.
// On Linux: the NVIDIA driver's NGX (libnvidia-ngx.so.1) as loaded at run time, declared below.

#pragma once

#ifdef _WIN32
#include <windows.h>

#include "video_core/vulkan_common/vulkan_wrapper.h"

#include <sl.h>
#include <sl_dlss.h>
#include <sl_helpers_vk.h>

namespace Vulkan::Streamline {

struct Api {
    PFun_slInit* init{};
    PFun_slShutdown* shutdown{};
    PFun_slIsFeatureSupported* is_feature_supported{};
    PFun_slGetFeatureRequirements* get_feature_requirements{};
    PFun_slGetFeatureVersion* get_feature_version{};
    PFun_slGetFeatureFunction* get_feature_function{};
    PFun_slGetNewFrameToken* get_new_frame_token{};
    PFun_slSetConstants* set_constants{};
    PFun_slEvaluateFeature* evaluate_feature{};
    PFun_slFreeResources* free_resources{};
    PFun_slSetVulkanInfo* set_vulkan_info{};
};

/// The loaded functions; only valid while IsDlssAvailable().
[[nodiscard]] const Api& GetApi();

} // namespace Vulkan::Streamline

#else // !_WIN32

#include <cstdint>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

// Fritchii's own declarations of the NGX entry points and structures the NVIDIA driver's
// libnvidia-ngx.so.1 exports (C ABI, x86-64 System V). No NVIDIA header is used: these are just the
// call shapes needed to talk to the user's installed driver, loaded with dlopen at run time.

namespace Vulkan::Streamline {

using NgxResult = u32;
constexpr NgxResult NgxSuccess = 0x1u;
[[nodiscard]] constexpr bool NgxSucceeded(NgxResult result) noexcept {
    return (result & 0xFFF00000u) != 0xBAD00000u;
}
[[nodiscard]] constexpr bool NgxFailed(NgxResult result) noexcept {
    return !NgxSucceeded(result);
}

enum class NgxFeature : u32 {
    SuperSampling = 1,
};

/// DLSS quality modes, as NGX numbers them.
enum class NgxPerfQuality : int {
    MaxPerf = 0,
    Balanced = 1,
    MaxQuality = 2,
    UltraPerformance = 3,
    UltraQuality = 4,
    DLAA = 5,
};

/// DLSS model presets, as NGX numbers them (A = 1 ... M = 13).
enum class NgxDlssPreset : u32 {
    Default = 0,
    J = 10,
    K = 11,
    L = 12,
    M = 13,
};

enum NgxDlssFeatureFlags : int {
    NgxDlssFlagNone = 0,
    NgxDlssFlagIsHDR = 1 << 0,
    NgxDlssFlagMVLowRes = 1 << 1,
    NgxDlssFlagMVJittered = 1 << 2,
    NgxDlssFlagDepthInverted = 1 << 3,
    NgxDlssFlagAutoExposure = 1 << 6,
};

enum class NgxLoggingLevel : u32 {
    Off = 0,
    On = 1,
    Verbose = 2,
};

struct NgxHandle {
    u32 id;
};

/// The driver's parameter block is a C++ object whose virtual table holds, in this order, eight
/// setters (unsigned long long, float, double, unsigned int, int, two Direct3D resources, void*)
/// and the eight matching getters. Plain function pointers taking the object first match it.
struct NgxParameterVtable {
    void (*SetULL)(void*, const char*, unsigned long long);
    void (*SetF)(void*, const char*, float);
    void (*SetD)(void*, const char*, double);
    void (*SetUI)(void*, const char*, unsigned int);
    void (*SetI)(void*, const char*, int);
    void (*SetD3d11Resource)(void*, const char*, void*);
    void (*SetD3d12Resource)(void*, const char*, void*);
    void (*SetVoidPointer)(void*, const char*, void*);
    NgxResult (*GetULL)(void*, const char*, unsigned long long*);
    NgxResult (*GetF)(void*, const char*, float*);
    NgxResult (*GetD)(void*, const char*, double*);
    NgxResult (*GetUI)(void*, const char*, unsigned int*);
    NgxResult (*GetI)(void*, const char*, int*);
    NgxResult (*GetD3d11Resource)(void*, const char*, void**);
    NgxResult (*GetD3d12Resource)(void*, const char*, void**);
    NgxResult (*GetVoidPointer)(void*, const char*, void**);
};

struct NgxParameter {
    const NgxParameterVtable* vtable;

    void SetF(const char* name, float value) {
        vtable->SetF(this, name, value);
    }
    void SetUI(const char* name, unsigned int value) {
        vtable->SetUI(this, name, value);
    }
    void SetI(const char* name, int value) {
        vtable->SetI(this, name, value);
    }
    void SetVoidPointer(const char* name, void* value) {
        vtable->SetVoidPointer(this, name, value);
    }
    NgxResult GetF(const char* name, float* value) {
        return vtable->GetF(this, name, value);
    }
    NgxResult GetUI(const char* name, unsigned int* value) {
        return vtable->GetUI(this, name, value);
    }
    NgxResult GetI(const char* name, int* value) {
        return vtable->GetI(this, name, value);
    }
    NgxResult GetVoidPointer(const char* name, void** value) {
        return vtable->GetVoidPointer(this, name, value);
    }
};

struct NgxPathListInfo {
    const wchar_t* const* path;
    unsigned int length;
};

using NgxAppLogCallback = void (*)(const char* message, NgxLoggingLevel level, NgxFeature source);

struct NgxLoggingInfo {
    NgxAppLogCallback callback;
    NgxLoggingLevel min_level;
    bool disable_other_sinks;
};

struct NgxFeatureCommonInfo {
    NgxPathListInfo path_list;
    void* internal_data;
    NgxLoggingInfo logging;
};

/// The application identifier is a tagged union (type, then project id, engine type and engine
/// version); laid out flat here with the same offsets.
struct NgxFeatureDiscoveryInfo {
    u32 sdk_version;
    NgxFeature feature_id;
    u32 identifier_type; ///< 1: project id
    const char* project_id;
    u32 engine_type; ///< 0: custom engine
    const char* engine_version;
    const wchar_t* app_data_path;
    const NgxFeatureCommonInfo* feature_info;
};

struct NgxImageViewInfoVk {
    VkImageView image_view;
    VkImage image;
    VkImageSubresourceRange subresource_range;
    VkFormat format;
    unsigned int width;
    unsigned int height;
};

struct NgxBufferInfoVk {
    VkBuffer buffer;
    unsigned int size_in_bytes;
};

struct NgxResourceVk {
    union {
        NgxImageViewInfoVk image_view_info;
        NgxBufferInfoVk buffer_info;
    };
    u32 type; ///< 0: image view
    bool read_write;
};

inline NgxResourceVk MakeNgxImageViewResource(VkImageView view, VkImage image,
                                              VkImageSubresourceRange range, VkFormat format,
                                              u32 width, u32 height, bool read_write) {
    NgxResourceVk resource{};
    resource.image_view_info = {view, image, range, format, width, height};
    resource.type = 0;
    resource.read_write = read_write;
    return resource;
}

/// The NGX interface version these declarations follow.
constexpr u32 NGX_SDK_VERSION = 0x0000015u;

using PFN_NgxVulkanInitProjectIdExt =
    NgxResult (*)(const char* project_id, u32 engine_type, const char* engine_version,
                  const wchar_t* app_data_path, VkInstance instance, VkPhysicalDevice physical,
                  VkDevice device, PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa,
                  u32 sdk_version, const NgxFeatureCommonInfo* feature_info);
using PFN_NgxVulkanShutdown1 = NgxResult (*)(VkDevice device, int* out_ref_count);
using PFN_NgxVulkanGetCapabilityParameters = NgxResult (*)(NgxParameter** out_params);
using PFN_NgxVulkanDestroyParameters = NgxResult (*)(NgxParameter* params);
using PFN_NgxVulkanCreateFeature1 = NgxResult (*)(VkDevice device, VkCommandBuffer cmd,
                                                  NgxFeature feature, NgxParameter* params,
                                                  NgxHandle** out_handle);
using PFN_NgxVulkanEvaluateFeature = NgxResult (*)(VkCommandBuffer cmd, const NgxHandle* handle,
                                                   const NgxParameter* params, void* callback);
using PFN_NgxVulkanReleaseFeature = NgxResult (*)(NgxHandle* handle);
using PFN_NgxVulkanGetFeatureInstanceExtensionRequirements =
    NgxResult (*)(const NgxFeatureDiscoveryInfo* info, uint32_t* count,
                  VkExtensionProperties** extensions);
/// DLSS's render size query, found in the capability parameters ("DLSSOptimalSettingsCallback").
using PFN_NgxDlssOptimalSettingsCallback = NgxResult (*)(NgxParameter* params);

struct NgxApi {
    PFN_NgxVulkanInitProjectIdExt init_project_id_ext{};
    PFN_NgxVulkanShutdown1 shutdown1{};
    PFN_NgxVulkanGetCapabilityParameters get_capability_parameters{};
    PFN_NgxVulkanDestroyParameters destroy_parameters{};
    PFN_NgxVulkanCreateFeature1 create_feature1{};
    PFN_NgxVulkanEvaluateFeature evaluate_feature{};
    PFN_NgxVulkanReleaseFeature release_feature{};
    PFN_NgxVulkanGetFeatureInstanceExtensionRequirements get_instance_extensions{};
};

/// The loaded functions; only valid while IsDlssAvailable().
[[nodiscard]] const NgxApi& GetNgxApi();

/// The capability parameter block every DLSS call passes; only valid while IsDlssAvailable().
[[nodiscard]] NgxParameter* GetNgxParameters();

} // namespace Vulkan::Streamline

#endif // _WIN32
