// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include "common/settings.h"

/// The picture shown in the window is bigger than the window (a 1440p or 4K picture on a 1080p
/// screen, 8K on a 4K one): set by the display layer each frame, read by the Auto filter.
inline std::atomic<bool> g_present_picture_bigger{false};

/// Auto: Area when the picture is bigger than the window (an exact average of every pixel a screen
/// pixel covers: no shimmer), FSR 1 with its sharpening otherwise (owner 2026-11-21, v6.5: FSR 1 is
/// also the default filter; ScaleForce before). FSR 1 is an upscaler, which skips pixels when
/// shrinking, so Auto keeps Area for a picture bigger than the window.
static inline Settings::ScalingFilter ResolveScalingFilter(Settings::ScalingFilter filter) {
    if (filter != Settings::ScalingFilter::Auto) {
        return filter;
    }
    return g_present_picture_bigger.load(std::memory_order_relaxed)
               ? Settings::ScalingFilter::Area
               : Settings::ScalingFilter::Fsr;
}

static inline Settings::ScalingFilter GetScalingFilter() {
    return ResolveScalingFilter(Settings::values.scaling_filter.GetValue());
}

static inline Settings::AntiAliasing GetAntiAliasing() {
    return Settings::values.anti_aliasing.GetValue();
}

static inline Settings::ScalingFilter GetScalingFilterForAppletCapture() {
    return Settings::ScalingFilter::Bilinear;
}

static inline Settings::AntiAliasing GetAntiAliasingForAppletCapture() {
    return Settings::AntiAliasing::None;
}

struct PresentFilters {
    Settings::ScalingFilter (*get_scaling_filter)();
    Settings::AntiAliasing (*get_anti_aliasing)();
    /// The window's filters: only these layers set g_present_picture_bigger (the Auto filter).
    bool display = false;
};

constexpr PresentFilters PresentFiltersForDisplay{
    .get_scaling_filter = &GetScalingFilter,
    .get_anti_aliasing = &GetAntiAliasing,
    .display = true,
};

constexpr PresentFilters PresentFiltersForAppletCapture{
    .get_scaling_filter = &GetScalingFilterForAppletCapture,
    .get_anti_aliasing = &GetAntiAliasingForAppletCapture,
};
