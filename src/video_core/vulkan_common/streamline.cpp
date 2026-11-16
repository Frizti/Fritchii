// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVIDIA DLSS through NVIDIA Streamline, with Streamline's manual hooking. On Linux: through the
// NVIDIA driver's NGX (libnvidia-ngx.so.1), loaded at run time.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <fmt/format.h>

#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/vulkan_common/streamline.h"
#include "video_core/vulkan_common/streamline_api.h"

#ifdef _WIN32
#include <sl_security.h>

#include <softpub.h>
#include <wintrust.h>
#else
#include <fstream>
#include <mutex>

#include <dlfcn.h>
#include <openssl/evp.h>
#endif

namespace Vulkan::Streamline {
#ifdef _WIN32
namespace {

// A custom engine names itself with its own project id; an application id from NVIDIA is only
// needed without an engine type. Keep it fixed: the driver may key behaviour off it.
constexpr const char* PROJECT_ID = "8d3e2f9a-6b41-4c7e-9a52-1f0c3b7d5e64";
constexpr const wchar_t* INTERPOSER = L"sl.interposer.dll";
constexpr const wchar_t* MODEL = L"nvngx_dlss.dll";
// The first DLSS with preset K.
constexpr u32 MIN_DLSS_MAJOR = 310;

using Version = std::array<u32, 4>;

struct State {
    HMODULE module{};
    Api api{};
    std::wstring plugin_dir;
    std::wstring model_dir;
    std::vector<std::string> instance_extensions;
    std::vector<std::string> device_extensions;
    bool started{};
    bool attached{};
};

State& GetState() {
    static State state;
    return state;
}

int LogLevel() {
    const char* const value = std::getenv("EDEN_NX2_DLSS_LOG");
    return value ? std::atoi(value) : 0;
}

void LogFromStreamline(sl::LogType type, const char* message) {
    std::string_view text{message ? message : ""};
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    switch (type) {
    case sl::LogType::eError:
        LOG_ERROR(Render_Vulkan, "Streamline: {}", text);
        break;
    case sl::LogType::eWarn:
        LOG_WARNING(Render_Vulkan, "Streamline: {}", text);
        break;
    default:
        if (LogLevel() > 0) {
            LOG_INFO(Render_Vulkan, "Streamline: {}", text);
        }
        break;
    }
}

/// Where the user may put sl.interposer.dll: EDEN_STREAMLINE_DIR, the user folder's "streamline"
/// folder, then a "streamline" folder next to the executable.
std::vector<std::filesystem::path> SearchDirs() {
    std::vector<std::filesystem::path> dirs;
    if (const char* const dir = std::getenv("EDEN_STREAMLINE_DIR"); dir && *dir) {
        dirs.emplace_back(dir);
    }
    dirs.push_back(UserFolder());
    dirs.push_back(Common::FS::GetExeDirectory() / "streamline");
    return dirs;
}

template <typename T>
bool Resolve(HMODULE module, const char* name, T*& function) {
    function = reinterpret_cast<T*>(GetProcAddress(module, name));
    if (!function) {
        LOG_ERROR(Render_Vulkan, "sl.interposer.dll has no {}", name);
    }
    return function != nullptr;
}

std::optional<Version> ReadVersion(const std::filesystem::path& path) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) {
        return std::nullopt;
    }
    std::vector<u8> data(size);
    VS_FIXEDFILEINFO* info{};
    UINT length = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info) {
        return std::nullopt;
    }
    return Version{HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                   HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS)};
}

std::string VersionText(const std::filesystem::path& path) {
    const std::optional<Version> version = path.empty() ? std::nullopt : ReadVersion(path);
    if (!version) {
        return {};
    }
    const Version& v = *version;
    return v[3] == 0 ? fmt::format("{}.{}.{}", v[0], v[1], v[2])
                     : fmt::format("{}.{}.{}.{}", v[0], v[1], v[2], v[3]);
}

/// The NVIDIA app keeps every DLSS it downloads under ProgramData; pick the newest.
std::filesystem::path FindAppModel() {
    const wchar_t* const program_data = _wgetenv(L"ProgramData");
    if (!program_data) {
        return {};
    }
    const std::filesystem::path root =
        std::filesystem::path{program_data} / L"NVIDIA" / L"NGX" / L"models" / L"dlss" / L"versions";
    std::filesystem::path best_path;
    Version best{};
    std::error_code ec;
    for (const auto& version_dir : std::filesystem::directory_iterator(root, ec)) {
        std::error_code files_ec;
        for (const auto& file :
             std::filesystem::directory_iterator(version_dir.path() / L"files", files_ec)) {
            if (file.path().extension() != L".bin") {
                continue;
            }
            const std::optional<Version> version = ReadVersion(file.path());
            if (version && (*version)[0] >= MIN_DLSS_MAJOR && *version > best) {
                best = *version;
                best_path = file.path();
            }
        }
    }
    return best_path;
}

/// Copies the NVIDIA app's DLSS into our folder as nvngx_dlss.dll, the name Streamline loads.
std::filesystem::path UseAppModel(const DlssFiles& files) {
    const std::filesystem::path dir = UserFolder() / L"nvidia-app";
    const std::filesystem::path target = dir / MODEL;
    std::error_code ec;
    const bool current = std::filesystem::is_regular_file(target, ec) &&
                         std::filesystem::file_size(target, ec) ==
                             std::filesystem::file_size(files.app_model, ec) &&
                         ReadVersion(target) == ReadVersion(files.app_model);
    if (!current) {
        if (!IsSignedByNvidia(files.app_model)) {
            LOG_WARNING(Render_Vulkan, "The NVIDIA app's DLSS {} is not signed by NVIDIA",
                        Common::FS::PathToUTF8String(files.app_model));
            return {};
        }
        std::filesystem::create_directories(dir, ec);
        std::filesystem::copy_file(files.app_model, target,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            LOG_WARNING(Render_Vulkan, "Could not copy the NVIDIA app's DLSS to {}: {}",
                        Common::FS::PathToUTF8String(target), ec.message());
            return {};
        }
    }
    if (!IsSignedByNvidia(target)) {
        LOG_WARNING(Render_Vulkan, "Cached DLSS {} is not signed by NVIDIA",
                    Common::FS::PathToUTF8String(target));
        return {};
    }
    return dir;
}

bool Load(State& state, const std::filesystem::path& path) {
    if (!IsStreamlineModule(path)) {
        LOG_ERROR(Render_Vulkan, "{} is not signed by NVIDIA; not loading it",
                  Common::FS::PathToUTF8String(path));
        return false;
    }
    state.module = LoadLibraryW(path.c_str());
    if (!state.module) {
        LOG_ERROR(Render_Vulkan, "Could not load {} (error {})", Common::FS::PathToUTF8String(path),
                  GetLastError());
        return false;
    }
    Api& api = state.api;
    const HMODULE m = state.module;
    if (Resolve(m, "slInit", api.init) && Resolve(m, "slShutdown", api.shutdown) &&
        Resolve(m, "slIsFeatureSupported", api.is_feature_supported) &&
        Resolve(m, "slGetFeatureRequirements", api.get_feature_requirements) &&
        Resolve(m, "slGetFeatureVersion", api.get_feature_version) &&
        Resolve(m, "slGetFeatureFunction", api.get_feature_function) &&
        Resolve(m, "slGetNewFrameToken", api.get_new_frame_token) &&
        Resolve(m, "slSetConstants", api.set_constants) &&
        Resolve(m, "slEvaluateFeature", api.evaluate_feature) &&
        Resolve(m, "slFreeResources", api.free_resources) &&
        Resolve(m, "slSetVulkanInfo", api.set_vulkan_info)) {
        return true;
    }
    FreeLibrary(state.module);
    state.module = nullptr;
    state.api = {};
    return false;
}

std::vector<std::string> Names(uint32_t count, const char** names) {
    std::vector<std::string> result;
    for (uint32_t i = 0; i < count; ++i) {
        result.emplace_back(names[i]);
    }
    return result;
}

} // Anonymous namespace

std::filesystem::path UserFolder() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::StreamlineDir);
}

bool IsStreamlineModule(const std::filesystem::path& path) {
    return sl::security::verifyEmbeddedSignature(path.c_str());
}

bool IsSignedByNvidia(const std::filesystem::path& path) {
    const std::wstring file_path = std::filesystem::absolute(path).wstring();
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = file_path.c_str();
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    bool valid = WinVerifyTrust(nullptr, &action, &data) == ERROR_SUCCESS;
    if (valid) {
        CRYPT_PROVIDER_DATA* const provider = WTHelperProvDataFromStateData(data.hWVTStateData);
        CRYPT_PROVIDER_SGNR* const signer =
            provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT* const cert = signer ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
        wchar_t name[128]{};
        valid = cert &&
                CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name,
                                   static_cast<DWORD>(std::size(name))) > 1 &&
                std::wstring_view{name} == L"NVIDIA Corporation";
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &data);
    return valid;
}

DlssFiles FindDlssFiles() {
    DlssFiles files;
    files.folder = UserFolder();
    std::error_code ec;
    for (const std::filesystem::path& dir : SearchDirs()) {
        if (std::filesystem::is_regular_file(dir / INTERPOSER, ec)) {
            files.folder = dir;
            files.interposer = dir / INTERPOSER;
            break;
        }
    }
    if (std::filesystem::is_regular_file(files.folder / MODEL, ec)) {
        files.model = files.folder / MODEL;
    }
    files.app_model = FindAppModel();
    files.interposer_version = VersionText(files.interposer);
    files.model_version = VersionText(files.model);
    files.app_model_version = VersionText(files.app_model);
    return files;
}

bool Start() {
    State& state = GetState();
    if (state.started) {
        return true;
    }
    if (!Settings::values.nvidia_dlss.GetValue()) {
        return false;
    }
    if (!state.module) {
        const DlssFiles files = FindDlssFiles();
        if (!files.model.empty() && !IsSignedByNvidia(files.model)) {
            LOG_WARNING(Render_Vulkan, "DLSS model {} is not signed by NVIDIA; using FSR 3.1",
                        Common::FS::PathToUTF8String(files.model));
            return false;
        }
        if (files.interposer.empty()) {
            LOG_WARNING(Render_Vulkan,
                        "NVIDIA DLSS is on, but sl.interposer.dll (NVIDIA Streamline) is not in "
                        "{} (the Graphics settings can download it); using FSR 3.1",
                        Common::FS::PathToUTF8String(files.folder));
            return false;
        }
        if (!Load(state, files.interposer)) {
            return false;
        }
        state.plugin_dir = files.folder.wstring();
        state.model_dir.clear();
        if (files.model.empty() && !files.app_model.empty()) {
            if (const std::filesystem::path dir = UseAppModel(files); !dir.empty()) {
                state.model_dir = dir.wstring();
                LOG_INFO(Render_Vulkan, "Using the NVIDIA app's DLSS {}", files.app_model_version);
            }
        } else if (files.model.empty()) {
            LOG_WARNING(Render_Vulkan, "No nvngx_dlss.dll in {} and no DLSS from the NVIDIA app",
                        Common::FS::PathToUTF8String(files.folder));
        }
    }

    // Streamline's plugins come from the loader's folder or the NVIDIA app's downloads (OTA); the
    // DLSS model from the loader's folder, else the NVIDIA app's copy.
    const wchar_t* plugin_paths[] = {state.plugin_dir.c_str(), state.model_dir.c_str()};
    const sl::Feature features[] = {sl::kFeatureDLSS};
    sl::Preferences preferences{};
    preferences.logLevel = LogLevel() >= 2 ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    preferences.logMessageCallback = LogFromStreamline;
    preferences.pathsToPlugins = plugin_paths;
    preferences.numPathsToPlugins = state.model_dir.empty() ? 1 : 2;
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                        sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eAllowOTA |
                        sl::PreferenceFlags::eLoadDownloadedPlugins;
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = 1;
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "1.0";
    preferences.projectId = PROJECT_ID;
    preferences.renderAPI = sl::RenderAPI::eVulkan;
    const Api& api = state.api;
    if (const sl::Result result = api.init(preferences, sl::kSDKVersion);
        result != sl::Result::eOk) {
        LOG_ERROR(Render_Vulkan, "NVIDIA Streamline failed to start (result {}); using FSR 3.1",
                  static_cast<int>(result));
        return false;
    }
    sl::FeatureRequirements requirements{};
    if (const sl::Result result = api.get_feature_requirements(sl::kFeatureDLSS, requirements);
        result != sl::Result::eOk) {
        LOG_WARNING(Render_Vulkan,
                    "Streamline found no DLSS plugin (result {}); put sl.dlss.dll next to "
                    "sl.interposer.dll or install the NVIDIA app. Using FSR 3.1",
                    static_cast<int>(result));
        api.shutdown();
        return false;
    }
    if (requirements.vkNumComputeQueuesRequired != 0 ||
        requirements.vkNumGraphicsQueuesRequired != 0) {
        LOG_WARNING(Render_Vulkan, "This DLSS wants extra Vulkan queues; using FSR 3.1");
        api.shutdown();
        return false;
    }
    state.instance_extensions =
        Names(requirements.vkNumInstanceExtensions, requirements.vkInstanceExtensions);
    state.device_extensions =
        Names(requirements.vkNumDeviceExtensions, requirements.vkDeviceExtensions);
    // The KHR and EXT buffer device address extensions can't both be enabled.
    const auto& device_extensions = state.device_extensions;
    if (std::ranges::find(device_extensions, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) !=
        device_extensions.end()) {
        std::erase(state.device_extensions, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    }
    sl::FeatureVersion version{};
    api.get_feature_version(sl::kFeatureDLSS, version);
    LOG_INFO(Render_Vulkan, "NVIDIA Streamline {} started from {} (DLSS {})",
             version.versionSL.toStr(),
             Common::FS::PathToUTF8String(std::filesystem::path{state.plugin_dir}),
             version.versionNGX.toStr());
    state.started = true;
    return true;
}

bool WantsDevice() {
    const State& state = GetState();
    return state.started && !state.attached;
}

std::vector<std::string> InstanceExtensions() {
    const State& state = GetState();
    return state.started ? state.instance_extensions : std::vector<std::string>{};
}

std::vector<std::string> DeviceExtensions() {
    const State& state = GetState();
    return state.started ? state.device_extensions : std::vector<std::string>{};
}

bool AttachDevice(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                  u32 graphics_family, vk::DeviceDispatch& dld) {
    State& state = GetState();
    if (!WantsDevice()) {
        return false;
    }
    sl::VulkanInfo info{};
    info.device = device;
    info.instance = instance;
    info.physicalDevice = physical;
    info.computeQueueFamily = graphics_family;
    info.graphicsQueueFamily = graphics_family;
    if (const sl::Result result = state.api.set_vulkan_info(info); result != sl::Result::eOk) {
        LOG_ERROR(Render_Vulkan, "Streamline did not take the Vulkan device (result {})",
                  static_cast<int>(result));
        Stop();
        return false;
    }
    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = physical;
    if (const sl::Result result = state.api.is_feature_supported(sl::kFeatureDLSS, adapter);
        result != sl::Result::eOk) {
        LOG_WARNING(Render_Vulkan,
                    "NVIDIA DLSS is not supported on this GPU or driver (Streamline result {}); "
                    "using FSR 3.1",
                    static_cast<int>(result));
        Stop();
        return false;
    }
    // Manual hooking: Streamline does its per-frame work in these.
    const auto hook = [&](auto& function, const char* name) {
        if (const FARPROC proxy = GetProcAddress(state.module, name)) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(proxy);
        } else {
            LOG_WARNING(Render_Vulkan, "sl.interposer.dll has no {}", name);
        }
    };
    hook(dld.vkCreateSwapchainKHR, "vkCreateSwapchainKHR");
    hook(dld.vkDestroySwapchainKHR, "vkDestroySwapchainKHR");
    hook(dld.vkGetSwapchainImagesKHR, "vkGetSwapchainImagesKHR");
    hook(dld.vkAcquireNextImageKHR, "vkAcquireNextImageKHR");
    hook(dld.vkQueuePresentKHR, "vkQueuePresentKHR");
    hook(dld.vkDeviceWaitIdle, "vkDeviceWaitIdle");
    state.attached = true;
    LOG_INFO(Render_Vulkan, "NVIDIA DLSS is available through Streamline");
    return true;
}

bool IsDlssAvailable() {
    const State& state = GetState();
    return state.started && state.attached;
}

void Stop() {
    State& state = GetState();
    if (!state.started) {
        return;
    }
    const sl::Result result = state.api.shutdown();
    state.started = false;
    state.attached = false;
    LOG_INFO(Render_Vulkan, "NVIDIA Streamline stopped (result {})", static_cast<int>(result));
}

const Api& GetApi() {
    return GetState().api;
}

#else // !_WIN32

// Linux: the NVIDIA driver ships the NGX core (libnvidia-ngx.so.1); DLSS itself is NVIDIA's
// libnvidia-ngx-dlss.so.<version>, which the player downloads from NVIDIA's DLSS repository on
// GitHub (Graphics settings). Both are loaded at run time and neither ships with Fritchii.
// Mechanism ported from TheSneakyBadger/FEFW-Stable-with-60-FPS-Patch PR #40.

namespace {

// Same project id as the Windows path.
constexpr const char* PROJECT_ID = "8d3e2f9a-6b41-4c7e-9a52-1f0c3b7d5e64";
// SHA-256 of libnvidia-ngx-dlss.so.310.9.16 from https://github.com/NVIDIA/DLSS/tree/v310.9.16
constexpr std::string_view LINUX_DLSS_SHA256 =
    "30982c937afed85c96b8d313323de9a650a2eec6de1bb0f714b21750f4ce98f6";
constexpr std::array<const char*, 2> NGX_LIBRARIES{"libnvidia-ngx.so.1", "libnvidia-ngx.so"};
// Where distributions put the driver's libraries (Fedora/openSUSE, Debian/Ubuntu, Arch, Debian's
// alternatives, NixOS). Anything else is found through the loader's own search below.
constexpr std::array<const char*, 6> NGX_DIRS{
    "/usr/lib64",
    "/usr/lib/x86_64-linux-gnu",
    "/usr/lib",
    "/usr/lib/nvidia",
    "/usr/lib/x86_64-linux-gnu/nvidia/current",
    "/run/opengl-driver/lib",
};

struct State {
    void* module{};
    NgxApi api{};
    std::wstring plugin_dir;
    std::wstring app_data_dir;
    std::array<const wchar_t*, 1> path_list{};
    NgxFeatureCommonInfo common{}; ///< Kept alive while NGX runs
    std::vector<std::string> instance_extensions;
    std::vector<std::string> device_extensions;
    VkDevice device{}; ///< Set while NGX is initialised on it
    NgxParameter* params{};
    bool started{};
    bool attached{};
};

State& GetState() {
    static State state;
    return state;
}

int LogLevel() {
    const char* const value = std::getenv("EDEN_NX2_DLSS_LOG");
    return value ? std::atoi(value) : 0;
}

void LogFromNgx(const char* message, NgxLoggingLevel level, NgxFeature /*source*/) {
    std::string_view text{message ? message : ""};
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    if (level == NgxLoggingLevel::On || LogLevel() > 0) {
        LOG_INFO(Render_Vulkan, "NGX: {}", text);
    }
}

/// NGX takes wide (UTF-32 on Linux) paths; decodes the UTF-8 path bytes.
std::wstring ToWide(const std::filesystem::path& path) {
    const std::string& text = path.native();
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        size_t length = 1;
        char32_t code = 0xFFFD;
        if (lead < 0x80) {
            code = lead;
        } else if ((lead >> 5) == 0x6) {
            length = 2;
            code = lead & 0x1F;
        } else if ((lead >> 4) == 0xE) {
            length = 3;
            code = lead & 0x0F;
        } else if ((lead >> 3) == 0x1E) {
            length = 4;
            code = lead & 0x07;
        }
        if (length > 1) {
            if (i + length > text.size()) {
                code = 0xFFFD;
                length = 1;
            } else {
                for (size_t k = 1; k < length; ++k) {
                    const auto next = static_cast<unsigned char>(text[i + k]);
                    if ((next & 0xC0) != 0x80) {
                        code = 0xFFFD;
                        length = k;
                        break;
                    }
                    code = (code << 6) | (next & 0x3F);
                }
            }
        }
        out.push_back(static_cast<wchar_t>(code));
        i += length;
    }
    return out;
}

std::string FileSha256Hex(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    EVP_MD_CTX* const context = EVP_MD_CTX_new();
    if (!context) {
        return {};
    }
    bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
    std::vector<char> buffer(1 << 16);
    while (ok && file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = file.gcount();
        if (got > 0) {
            ok = EVP_DigestUpdate(context, buffer.data(), static_cast<size_t>(got)) == 1;
        }
    }
    ok = ok && file.eof();
    std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
    unsigned int length = 0;
    ok = ok && EVP_DigestFinal_ex(context, hash.data(), &length) == 1;
    EVP_MD_CTX_free(context);
    if (!ok) {
        return {};
    }
    std::string hex;
    hex.reserve(length * 2);
    for (unsigned int i = 0; i < length; ++i) {
        hex += fmt::format("{:02x}", hash[i]);
    }
    return hex;
}

/// Where the DLSS model may be: FRITCHII_DLSS_DIR, the user folder's "streamline" folder, then a
/// "streamline" folder next to the executable or the AppImage.
std::vector<std::filesystem::path> SearchDirs() {
    std::vector<std::filesystem::path> dirs;
    if (const char* const dir = std::getenv("FRITCHII_DLSS_DIR"); dir && *dir) {
        dirs.emplace_back(dir);
    }
    dirs.push_back(UserFolder());
    std::error_code ec;
    if (const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        !ec) {
        dirs.push_back(exe.parent_path() / "streamline");
    }
    if (const char* const appimage = std::getenv("APPIMAGE"); appimage && *appimage) {
        dirs.push_back(std::filesystem::path{appimage}.parent_path() / "streamline");
    }
    return dirs;
}

/// The driver's NGX core, looked up once: the usual folders, then the loader's own search
/// (LD_LIBRARY_PATH, ld.so.cache, Flatpak's GL extension). Empty without NVIDIA's own driver
/// (nouveau, NVK, AMD, Intel).
const std::filesystem::path& NgxCorePath() {
    static const std::filesystem::path path = [] {
        std::error_code ec;
        for (const char* const dir : NGX_DIRS) {
            for (const char* const name : NGX_LIBRARIES) {
                const std::filesystem::path candidate = std::filesystem::path{dir} / name;
                if (std::filesystem::exists(candidate, ec)) {
                    return candidate;
                }
            }
        }
        for (const char* const name : NGX_LIBRARIES) {
            void* const handle = dlopen(name, RTLD_LAZY | RTLD_LOCAL);
            if (!handle) {
                continue;
            }
            std::filesystem::path found{name};
            Dl_info info{};
            if (void* const symbol = dlsym(handle, "NVSDK_NGX_VULKAN_Init_ProjectID_Ext");
                symbol && dladdr(symbol, &info) != 0 && info.dli_fname && *info.dli_fname) {
                found = info.dli_fname;
            }
            dlclose(handle);
            return found;
        }
        return std::filesystem::path{};
    }();
    return path;
}

/// The loaded NVIDIA kernel module's version ("580.95.05"), for the log and the settings panel.
std::string DriverVersion() {
    std::ifstream file("/sys/module/nvidia/version");
    std::string version;
    std::getline(file, version);
    while (!version.empty() && (version.back() == '\n' || version.back() == ' ')) {
        version.pop_back();
    }
    return version;
}

template <typename T>
bool Resolve(void* module, const char* name, T& function) {
    function = reinterpret_cast<T>(dlsym(module, name));
    if (!function) {
        LOG_ERROR(Render_Vulkan, "The NVIDIA driver's NGX has no {}", name);
    }
    return function != nullptr;
}

bool Load(State& state, const std::filesystem::path& path) {
    state.module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    for (const char* const name : NGX_LIBRARIES) {
        if (state.module) {
            break;
        }
        state.module = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    }
    if (!state.module) {
        const char* const error = dlerror();
        LOG_ERROR(Render_Vulkan, "Could not load the NVIDIA driver's {} ({})",
                  Common::FS::PathToUTF8String(path), error ? error : "unknown error");
        return false;
    }
    NgxApi& api = state.api;
    void* const m = state.module;
    if (Resolve(m, "NVSDK_NGX_VULKAN_Init_ProjectID_Ext", api.init_project_id_ext) &&
        Resolve(m, "NVSDK_NGX_VULKAN_Shutdown1", api.shutdown1) &&
        Resolve(m, "NVSDK_NGX_VULKAN_GetCapabilityParameters", api.get_capability_parameters) &&
        Resolve(m, "NVSDK_NGX_VULKAN_DestroyParameters", api.destroy_parameters) &&
        Resolve(m, "NVSDK_NGX_VULKAN_CreateFeature1", api.create_feature1) &&
        Resolve(m, "NVSDK_NGX_VULKAN_EvaluateFeature", api.evaluate_feature) &&
        Resolve(m, "NVSDK_NGX_VULKAN_ReleaseFeature", api.release_feature) &&
        Resolve(m, "NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements",
                api.get_instance_extensions)) {
        return true;
    }
    dlclose(state.module);
    state.module = nullptr;
    state.api = {};
    return false;
}

/// Hashing the 57 MB model takes a moment: remember the answer per file, size and change time.
struct HashCheck {
    std::filesystem::path path;
    std::uintmax_t size{};
    std::filesystem::file_time_type changed{};
    bool valid{};
};
std::mutex hash_mutex;
std::vector<HashCheck> hash_checks;

} // Anonymous namespace

std::filesystem::path UserFolder() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::StreamlineDir);
}

bool IsStreamlineModule(const std::filesystem::path& /*path*/) {
    // No Streamline on Linux.
    return false;
}

bool IsSignedByNvidia(const std::filesystem::path& path) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size != LINUX_DLSS_SIZE) {
        return false;
    }
    const std::filesystem::file_time_type changed = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return false;
    }
    std::filesystem::path key = std::filesystem::absolute(path, ec);
    if (ec) {
        key = path;
    }
    {
        std::scoped_lock lock{hash_mutex};
        for (const HashCheck& check : hash_checks) {
            if (check.path == key && check.size == size && check.changed == changed) {
                return check.valid;
            }
        }
    }
    const bool valid = FileSha256Hex(path) == LINUX_DLSS_SHA256;
    if (!valid) {
        LOG_WARNING(Render_Vulkan, "{} does not match NVIDIA's DLSS {} (SHA-256)",
                    Common::FS::PathToUTF8String(path), LINUX_DLSS_VERSION);
    }
    std::scoped_lock lock{hash_mutex};
    std::erase_if(hash_checks, [&](const HashCheck& check) { return check.path == key; });
    hash_checks.push_back({key, size, changed, valid});
    return valid;
}

DlssFiles FindDlssFiles() {
    DlssFiles files;
    files.folder = UserFolder();
    files.interposer = NgxCorePath();
    if (!files.interposer.empty()) {
        files.interposer_version = DriverVersion();
    }
    std::error_code ec;
    for (const std::filesystem::path& dir : SearchDirs()) {
        const std::filesystem::path model = dir / LINUX_DLSS_FILE;
        if (std::filesystem::is_regular_file(model, ec) && IsSignedByNvidia(model)) {
            files.folder = dir;
            files.model = model;
            files.model_version = LINUX_DLSS_VERSION;
            break;
        }
    }
    return files;
}

bool Start() {
    State& state = GetState();
    if (state.started) {
        return true;
    }
    if (!Settings::values.nvidia_dlss.GetValue()) {
        return false;
    }
    const DlssFiles files = FindDlssFiles();
    if (files.interposer.empty()) {
        LOG_WARNING(Render_Vulkan,
                    "NVIDIA DLSS is on, but the NVIDIA driver's libnvidia-ngx.so.1 was not found "
                    "(DLSS on Linux needs NVIDIA's own driver, not nouveau or NVK); using FSR 3.1");
        return false;
    }
    if (files.model.empty()) {
        LOG_WARNING(Render_Vulkan,
                    "NVIDIA DLSS is on, but {} is not in {} (the Graphics settings can download "
                    "it); using FSR 3.1",
                    LINUX_DLSS_FILE, Common::FS::PathToUTF8String(files.folder));
        return false;
    }
    if (!state.module && !Load(state, files.interposer)) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(UserFolder(), ec);
    std::filesystem::path plugin_dir = std::filesystem::absolute(files.folder, ec);
    if (ec) {
        plugin_dir = files.folder;
    }
    std::filesystem::path app_data_dir = std::filesystem::absolute(UserFolder(), ec);
    if (ec) {
        app_data_dir = UserFolder();
    }
    state.plugin_dir = ToWide(plugin_dir);
    state.app_data_dir = ToWide(app_data_dir);
    state.path_list = {state.plugin_dir.c_str()};
    state.common = {};
    state.common.path_list.path = state.path_list.data();
    state.common.path_list.length = static_cast<unsigned int>(state.path_list.size());
    state.common.logging.callback = LogFromNgx;
    state.common.logging.min_level =
        LogLevel() >= 2 ? NgxLoggingLevel::Verbose : NgxLoggingLevel::On;

    NgxFeatureDiscoveryInfo discovery{};
    discovery.sdk_version = NGX_SDK_VERSION;
    discovery.feature_id = NgxFeature::SuperSampling;
    discovery.identifier_type = 1;
    discovery.project_id = PROJECT_ID;
    discovery.engine_type = 0;
    discovery.engine_version = "1.0";
    discovery.app_data_path = state.app_data_dir.c_str();
    discovery.feature_info = &state.common;
    uint32_t count = 0;
    VkExtensionProperties* extensions = nullptr;
    state.instance_extensions.clear();
    if (NgxSucceeded(state.api.get_instance_extensions(&discovery, &count, &extensions)) &&
        extensions) {
        for (uint32_t i = 0; i < count; ++i) {
            state.instance_extensions.emplace_back(extensions[i].extensionName);
        }
    } else {
        state.instance_extensions = {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};
    }
    // What NGX's DLSS needs on the device (NVIDIA's Vulkan guide for NGX).
    state.device_extensions = {
        "VK_NVX_binary_import",
        "VK_NVX_image_view_handle",
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
    };
    LOG_INFO(Render_Vulkan, "NVIDIA DLSS {} found in {}; NGX from {} (driver {})",
             files.model_version, Common::FS::PathToUTF8String(files.folder),
             Common::FS::PathToUTF8String(files.interposer),
             files.interposer_version.empty() ? "unknown" : files.interposer_version);
    state.started = true;
    return true;
}

bool WantsDevice() {
    const State& state = GetState();
    return state.started && !state.attached;
}

std::vector<std::string> InstanceExtensions() {
    const State& state = GetState();
    return state.started ? state.instance_extensions : std::vector<std::string>{};
}

std::vector<std::string> DeviceExtensions() {
    const State& state = GetState();
    return state.started ? state.device_extensions : std::vector<std::string>{};
}

bool AttachDevice(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                  u32 /*graphics_family*/, vk::DeviceDispatch& dld) {
    State& state = GetState();
    if (!WantsDevice()) {
        return false;
    }
    // NGX needs no swapchain hooks: it only records into the command buffers it is handed.
    const NgxResult result = state.api.init_project_id_ext(
        PROJECT_ID, 0u, "1.0", state.app_data_dir.c_str(), instance, physical, device,
        dld.vkGetInstanceProcAddr, dld.vkGetDeviceProcAddr, NGX_SDK_VERSION, &state.common);
    if (NgxFailed(result)) {
        LOG_WARNING(Render_Vulkan,
                    "NVIDIA DLSS could not start on this GPU or driver (NGX 0x{:x}); using FSR 3.1",
                    result);
        Stop();
        return false;
    }
    state.device = device;
    if (NgxFailed(state.api.get_capability_parameters(&state.params)) || !state.params) {
        state.params = nullptr;
        LOG_WARNING(Render_Vulkan, "NVIDIA NGX gave no DLSS parameters; using FSR 3.1");
        Stop();
        return false;
    }
    int available = 0;
    state.params->GetI("SuperSampling.Available", &available);
    if (available == 0) {
        int needs_driver = 0;
        unsigned int driver_major = 0;
        unsigned int driver_minor = 0;
        state.params->GetI("SuperSampling.NeedsUpdatedDriver", &needs_driver);
        state.params->GetUI("SuperSampling.MinDriverVersionMajor", &driver_major);
        state.params->GetUI("SuperSampling.MinDriverVersionMinor", &driver_minor);
        if (needs_driver != 0) {
            LOG_WARNING(Render_Vulkan,
                        "NVIDIA DLSS needs NVIDIA driver {}.{} or newer; using FSR 3.1",
                        driver_major, driver_minor);
        } else {
            LOG_WARNING(Render_Vulkan,
                        "NVIDIA DLSS is not available on this GPU or driver; using FSR 3.1");
        }
        Stop();
        return false;
    }
    state.attached = true;
    LOG_INFO(Render_Vulkan, "NVIDIA DLSS is available through the NVIDIA driver's NGX");
    return true;
}

bool IsDlssAvailable() {
    const State& state = GetState();
    return state.started && state.attached;
}

void Stop() {
    State& state = GetState();
    if (!state.started) {
        return;
    }
    if (state.params) {
        state.api.destroy_parameters(state.params);
        state.params = nullptr;
    }
    if (state.device) {
        int references = 0;
        state.api.shutdown1(state.device, &references);
        state.device = VK_NULL_HANDLE;
    }
    // The driver's library stays loaded, as the Windows path keeps sl.interposer.dll.
    state.started = false;
    state.attached = false;
    LOG_INFO(Render_Vulkan, "NVIDIA NGX stopped");
}

const NgxApi& GetNgxApi() {
    return GetState().api;
}

NgxParameter* GetNgxParameters() {
    return GetState().params;
}

#endif // _WIN32

} // namespace Vulkan::Streamline
