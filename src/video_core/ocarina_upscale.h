// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cmath>
#include <cstdlib>
#include <string_view>

#include "common/settings_enums.h"

namespace VideoCommon {

enum class OcarinaUpscaleBackend { Native, Fsr, Dlss };

constexpr const char* OcarinaBackendName(OcarinaUpscaleBackend backend) {
    switch (backend) {
    case OcarinaUpscaleBackend::Native:
        return "the game's upscale";
    case OcarinaUpscaleBackend::Fsr:
        return "FSR";
    case OcarinaUpscaleBackend::Dlss:
        return "NVIDIA DLSS";
    }
    return "";
}

namespace OcarinaSwitch {
/// A boot switch read once: unset or empty gives `fallback`.
inline int Read(const char* name, int fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::atoi(value) : fallback;
}
} // namespace OcarinaSwitch

/// FRITCHII_DLSS_SANITIZE (boot, 1 = on by default): the upscaler's colour input has NaN and
/// infinite texels set to 0 and is clamped to [0, 1] (the game's colour is already tonemapped).
/// DLSS spreads such a texel into a black block: the small black squares around far birds and
/// particles in Hyrule Field. 0 = the exact copy as before.
inline bool OcarinaDlssSanitize() {
    static const bool on = OcarinaSwitch::Read("FRITCHII_DLSS_SANITIZE", 1) != 0;
    return on;
}

/// FRITCHII_FSR4_JITTER (boot, 0 = off): AMD FSR 4 gets the camera jitter and texture LOD bias
/// FSR 3.1 uses. Off, FSR 4 takes the scene as the game draws it, without jitter or bias, as in
/// CheezyEmu, whose FSR 4 AMD testers report as working well (RX 9070 XT). FSR 3.1 and DLSS
/// are unchanged.
inline bool OcarinaFsr4Jitter() {
    static const bool on = OcarinaSwitch::Read("FRITCHII_FSR4_JITTER", 0) != 0;
    return on;
}

/// FRITCHII_FSR4_LINEAR (boot, 0 = off): AMD FSR 4 is told the colour is linear, as before.
/// Off, it is told the colour is perceptual (gamma encoded), which OoT's tonemapped colour is
/// (CheezyEmu's FSR 4 setup).
inline bool OcarinaFsr4LinearColor() {
    static const bool on = OcarinaSwitch::Read("FRITCHII_FSR4_LINEAR", 0) != 0;
    return on;
}

/// FRITCHII_OOT_CAMERA_GUEST (boot, 0 = off): the camera tracker reads vertex uniform slot 3 as
/// the game bound it, not as the draw's shader sees it. The shader's view leaves out slots it does
/// not use and stops at the last byte it reads, so draws that read less than the camera block gave
/// no camera and the upscaler stepped aside ("no scene camera this frame", e.g. when the camera
/// turns in the Water Temple). On, only the 0x1c0 bytes up to the end of the previous matrix
/// are needed.
inline bool OcarinaCameraFromGuest() {
    static const bool on = OcarinaSwitch::Read("FRITCHII_OOT_CAMERA_GUEST", 0) != 0;
    return on;
}
/// The camera bytes the tracker needs: through the previous view-projection matrix (0x180 + 64)
/// with FRITCHII_OOT_CAMERA_GUEST, the whole block as before without it.
inline constexpr unsigned OcarinaCameraBlockBytes = 0x230;
inline constexpr unsigned OcarinaCameraNeededBytes = 0x1c0;
inline unsigned OcarinaCameraReadBytes() {
    return OcarinaCameraFromGuest() ? OcarinaCameraNeededBytes : OcarinaCameraBlockBytes;
}

/// FRITCHII_JITTER_EDGE (boot, 1 = on by default): on AMD's drivers the jitter offsets are
/// Halton (2, 3) in [0, 1) instead of [-0.5, 0.5). AMD clips each viewport to a whole pixel, so a
/// negative offset dropped the last column and row of every jittered draw on half the frames
/// (a ripple along the right and bottom edges with FSR). Other drivers are unchanged.
inline bool OcarinaJitterEdgeFix() {
    static const bool on = OcarinaSwitch::Read("FRITCHII_JITTER_EDGE", 1) != 0;
    return on;
}

/// The camera jitter for a phase (from 1) in render pixels: Halton (2, 3), centred on 0, or in
/// [0, 1) for drivers that clip the viewport's fractional right and bottom edges.
inline std::array<float, 2> OcarinaJitter(unsigned index, bool truncated_viewport_edges) {
    const auto halton = [](unsigned value, unsigned base) {
        float fraction = 1.0f;
        float result = 0.0f;
        while (value > 0) {
            fraction /= static_cast<float>(base);
            result += fraction * static_cast<float>(value % base);
            value /= base;
        }
        return result;
    };
    const float center = truncated_viewport_edges ? 0.0f : 0.5f;
    return {halton(index, 2) - center, halton(index, 3) - center};
}

/// FRITCHII_OOT_LOD_RATIO (boot, 1 = by default): the texture LOD bias follows the render and
/// output sizes: FSR log2(render / output) - 1 (AMD), DLSS with its jitter log2(render / output)
/// (NVIDIA). 2: DLSS gets FSR's formula too, so the same sharpness as FSR (-1.58 for
/// 1440p -> 4K). 0: the fixed numbers as before (FSR -1.58, DLSS with jitter -0.58).
inline int OcarinaLodRatioMode() {
    static const int mode = OcarinaSwitch::Read("FRITCHII_OOT_LOD_RATIO", 1);
    return mode;
}

constexpr OcarinaUpscaleBackend SelectOcarinaUpscaler(bool fsr_enabled, bool dlss_enabled,
                                                    bool dlss_available,
                                                    std::string_view override) {
    if (override == "native") {
        return OcarinaUpscaleBackend::Native;
    }
    if (override == "fsr") {
        return OcarinaUpscaleBackend::Fsr;
    }
    if ((dlss_enabled || override == "dlss") && dlss_available) {
        return OcarinaUpscaleBackend::Dlss;
    }
    return fsr_enabled ? OcarinaUpscaleBackend::Fsr : OcarinaUpscaleBackend::Native;
}

/// Whether the "OoT anti-aliasing" choice asks for AMD FSR (3.1, or 4 with FSR 3.1 as fallback).
constexpr bool OcarinaFsrChosen(Settings::OotAntiAliasing choice) {
    return choice == Settings::OotAntiAliasing::Fsr3 || choice == Settings::OotAntiAliasing::Fsr4;
}

/// The backend for the "OoT anti-aliasing" choice and the NVIDIA DLSS setting. An FSR choice wins
/// over DLSS; with Game or No sharpening, DLSS replaces the game's pass when it's on and available
/// (as before FSR came in).
constexpr OcarinaUpscaleBackend SelectOcarinaUpscaler(Settings::OotAntiAliasing choice,
                                                    bool dlss_enabled, bool dlss_available,
                                                    std::string_view override) {
    const bool fsr = OcarinaFsrChosen(choice);
    return SelectOcarinaUpscaler(fsr, dlss_enabled && !fsr, dlss_available, override);
}

/// EDEN_NX2_DLSS_JITTER (boot): overrides the "DLSS jitter" setting. Unset: the setting (on by
/// default). 0 = off, 1 = on, 2 = on with the vertical sign flipped for DLSS (a test only: the
/// offset DLSS gets is the one the scene moved by, the convention of NVIDIA's own Streamline
/// sample and of AMD's FSR, so 1 is right). -1 when unset.
inline int OcarinaDlssJitterOverride() {
    static const int mode = OcarinaSwitch::Read("EDEN_NX2_DLSS_JITTER", -1);
    return mode;
}

/// Whether DLSS gets the camera jitter and the texture bias that goes with it (DLSS is made for
/// jittered input): the override when set, else the "DLSS jitter" setting.
constexpr bool OcarinaDlssJitterOn(int override_mode, bool setting) {
    return override_mode >= 0 ? override_mode != 0 : setting;
}
inline bool OcarinaDlssJitterOn(bool setting) {
    return OcarinaDlssJitterOn(OcarinaDlssJitterOverride(), setting);
}

/// EDEN_NX2_DLSS_JITTER=2: DLSS is told the offset with y flipped.
inline bool OcarinaDlssJitterFlipY() {
    return OcarinaDlssJitterOverride() == 2;
}

/// FRITCHII_DLSS_SCALED (boot, 0 = off): with DLSS or FSR picked for OoT, keep the player's
/// resolution scale instead of setting it to 1x for the session. The upscaler then takes the
/// scene at the scaled size (2x: four times the pixels, 0.5x: a quarter) and picks its mode from
/// the real render and output sizes. Off: the scale goes to 1x as before.
inline bool OcarinaScaledUpscale() {
    static const bool on = [] {
        const char* value = std::getenv("FRITCHII_DLSS_SCALED");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return on;
}

struct OcarinaExtent {
    unsigned width{};
    unsigned height{};
    constexpr bool operator==(const OcarinaExtent&) const = default;
};

/// What the temporal upscaler writes for one frame with the scene at the resolution scale.
/// `render`: the scene's real size (the scaled size when the scaler scaled it). `target`: the real
/// size of the game's upscale target. `native_aa`: the DLSS quality "Native" (the output keeps the
/// render size). `want_4k`: the 1440p pass with the 4K output. DLSS and FSR only enlarge, so a
/// scene larger than the target is upscaled to its own size (native AA) and the output pass
/// shrinks the result into the target: supersampling.
struct OcarinaScaledOutput {
    OcarinaExtent output;
    bool upscale_4k{};
};

constexpr OcarinaScaledOutput SelectOcarinaScaledOutput(OcarinaExtent render, OcarinaExtent target,
                                                        bool native_aa, bool want_4k) {
    const auto fits = [render](OcarinaExtent output) {
        return render.width <= output.width && render.height <= output.height;
    };
    if (native_aa) {
        return {render, false};
    }
    constexpr OcarinaExtent uhd{3840, 2160};
    if (want_4k && fits(uhd)) {
        return {uhd, true};
    }
    return {fits(target) ? target : render, false};
}

/// Texture LOD bias added to Ocarina's mipmapped samplers while FSR upscales. AMD recommends
/// log2(render / output) - 1: -1.58 for docked 1440p -> 4K. With the DLSS jitter, DLSS gets
/// NVIDIA's log2(render / output): -0.58. The fixed numbers of FRITCHII_OOT_LOD_RATIO=0.
inline float OcarinaFsrLodBias(OcarinaUpscaleBackend backend, bool dlss_jitter = false) {
    if (backend == OcarinaUpscaleBackend::Fsr) {
        return -1.58f;
    }
    return backend == OcarinaUpscaleBackend::Dlss && dlss_jitter ? -0.58f : 0.0f;
}

/// The bias for the render and output sizes (one side). `mode` 1: AMD's and NVIDIA's formulas,
/// 2: FSR's for DLSS too. DLSS gets a bias only with its jitter on (without jitter a sharper mip
/// only shimmers).
inline float OcarinaLodBiasForSizes(OcarinaUpscaleBackend backend, unsigned render_size,
                                    unsigned output_size, bool dlss_jitter, int mode) {
    if (render_size == 0 || output_size == 0 || backend == OcarinaUpscaleBackend::Native) {
        return 0.0f;
    }
    const float ratio = std::log2(static_cast<float>(render_size) /
                                  static_cast<float>(output_size));
    if (backend == OcarinaUpscaleBackend::Fsr) {
        return ratio - 1.0f;
    }
    if (!dlss_jitter) {
        return 0.0f;
    }
    return mode >= 2 ? ratio - 1.0f : ratio;
}

/// The "Texture sharpness" choice: Auto is the bias for the sizes (or the fixed numbers with
/// FRITCHII_OOT_LOD_RATIO=0), Sharper half a mip more, Custom the player's value as is (DLSS
/// without its jitter too: the player asked for it). Nothing for the game's own upscale.
inline float OcarinaTextureLodBias(OcarinaUpscaleBackend backend, unsigned render_size,
                                   unsigned output_size, bool dlss_jitter, int ratio_mode,
                                   Settings::OotTextureSharpness choice, float custom) {
    if (backend == OcarinaUpscaleBackend::Native) {
        return 0.0f;
    }
    if (choice == Settings::OotTextureSharpness::Custom) {
        return std::isfinite(custom) ? custom : 0.0f;
    }
    const float automatic =
        ratio_mode == 0
            ? OcarinaFsrLodBias(backend, dlss_jitter)
            : OcarinaLodBiasForSizes(backend, render_size, output_size, dlss_jitter, ratio_mode);
    if (choice != Settings::OotTextureSharpness::Sharper ||
        (backend == OcarinaUpscaleBackend::Dlss && !dlss_jitter)) {
        return automatic;
    }
    return automatic - 0.5f;
}

/// Whether a sampler with that bias keeps it for an image: only docked (handheld isn't measured),
/// not for images the GPU wrote (render targets would read a mip too sharp), and not once the
/// upscaler failed this session (the game's own pass runs again).
constexpr bool KeepOcarinaFsrLodBias(bool docked, bool upscaler_failed, bool gpu_written) {
    return docked && !upscaler_failed && !gpu_written;
}

} // namespace VideoCommon
