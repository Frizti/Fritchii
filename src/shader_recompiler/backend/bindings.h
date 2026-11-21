// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_types.h"

namespace Shader::Backend {

struct Bindings {
    u32 unified{};
    u32 uniform_buffer{};
    u32 storage_buffer{};
    u32 texture{};
    u32 image{};
    u32 texture_scaling_index{};
    u32 image_scaling_index{};
    /// Push-constant offset of the render area: 0 its own block, else a member at that offset,
    /// in the rescaling block when the stage uses both (NVN direct mode's layout)
    u32 render_area_offset{};
    /// Descriptor set the stage's resources are declared in (SPIR-V). 0 everywhere except with
    /// graphics pipeline libraries (EDEN_NX2_GPL), where each stage has its own set.
    u32 descriptor_set{};
};

} // namespace Shader::Backend
