// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <type_traits>
#include <typeindex>
#include <vector>
#include <QColor>
#include <QString>
#include <QWidget>
#include <qobjectdefs.h>
#include <vulkan/vulkan_core.h>
#include "common/common_types.h"
#include "common/settings_enums.h"
#include "qt_common/config/shared_translation.h"
#include "qt_common/util/vk.h"
#include "yuzu/configuration/configuration_shared.h"

class QPushButton;
class QEvent;
class QObject;
class QComboBox;
class QCheckBox;
class QLabel;
class AmdFsr4Panel;

namespace Settings {
enum class NvdecEmulation : u32;
enum class RendererBackend : u32;
} // namespace Settings

namespace Core {
class System;
}

namespace Ui {
class ConfigureGraphics;
}

namespace ConfigurationShared {
class Builder;
}

class ConfigureGraphics : public ConfigurationShared::Tab {
    Q_OBJECT

public:
    explicit ConfigureGraphics(
        const Core::System& system_, std::vector<VkDeviceInfo::Record>& records,
        const std::function<void()>& expose_compute_option,
        const std::function<void(Settings::AspectRatio, Settings::ResolutionSetup)>&
            update_aspect_ratio,
        std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group,
        const ConfigurationShared::Builder& builder, QWidget* parent = nullptr);
    ~ConfigureGraphics() override;

    void ApplyConfiguration() override;
    void SetConfiguration() override;

private:
    void changeEvent(QEvent* event) override;
    void RetranslateUI();

    void Setup(const ConfigurationShared::Builder& builder);

    void PopulateVSyncModeSelection(bool use_setting);
    void UpdateVsyncSetting() const;
    void UpdateBackgroundColorButton(QColor color);
    void UpdateAPILayout();
    void UpdateDeviceSelection(int device);
    void UpdateAmdFsr4Visibility();
    void UpdateDlssVisibility();
    /// Fritchii builds: OoT's real sizes in the DLSS quality names, and the line under the
    /// resolution while DLSS or FSR sets OoT's resolution.
    void SetupOcarinaUpscalerLabels();
    void UpdateOcarinaUpscalerLabels();

    void RetrieveVulkanDevices();

    /* Turns a Vulkan present mode into a textual string for a UI
     * (and eventually for a human to read) */
    const QString TranslateVSyncMode(VkPresentModeKHR mode,
                                     Settings::RendererBackend backend) const;

    Settings::RendererBackend GetCurrentGraphicsBackend() const;

    int FindIndex(u32 enumeration, int value) const;

    std::unique_ptr<Ui::ConfigureGraphics> ui;
    QColor bg_color;

    std::vector<std::function<void(bool)>> apply_funcs{};

    std::vector<VkDeviceInfo::Record>& records;
    std::vector<QString> vulkan_devices;
    std::vector<std::vector<VkPresentModeKHR>> device_present_modes;
    std::vector<VkPresentModeKHR>
        vsync_mode_combobox_enum_map{}; //< Keeps track of which present mode corresponds to which
                                        // selection in the combobox
    u32 vulkan_device{};
    const std::function<void()>& expose_compute_option;
    const std::function<void(Settings::AspectRatio, Settings::ResolutionSetup)> update_aspect_ratio;

    const Core::System& system;
    const ConfigurationShared::ComboboxTranslationMap& combobox_translations;

    QPushButton* api_restore_global_button;
    QComboBox* vulkan_device_combobox;
    QComboBox* api_combobox;
    QComboBox* vsync_mode_combobox;
    QPushButton* vsync_restore_global_button;
    QWidget* vulkan_device_widget;
    QWidget* api_widget;
    QComboBox* aspect_ratio_combobox;
    QComboBox* resolution_combobox;
    QComboBox* oot_aa_combobox{};          ///< The oot_anti_aliasing choice
    AmdFsr4Panel* amd_fsr4_panel{};       ///< Under it while AMD FSR 4 is picked
    QCheckBox* nvidia_dlss_checkbox{};     ///< The nvidia_dlss setting
    QWidget* nvidia_dlss_group{};          ///< The DLSS checkbox and NVIDIA's panel under it
    QComboBox* dlss_quality_combobox{};    ///< The nvidia_dlss_quality choice
    QLabel* resolution_note{};             ///< Fritchii: under the resolution choice
};
