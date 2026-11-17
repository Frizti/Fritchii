// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

#include "common/common_types.h"
#include "common/math_util.h"

namespace VideoCore::TestWatermark {

/// One block of the mark, top-left origin, in frame pixels.
struct Block {
    u32 x;
    u32 y;
    u32 width;
    u32 height;
};

struct Blocks {
    std::vector<Block> shadow; ///< Drawn first, dark
    std::vector<Block> text;   ///< Drawn over it, light
};

inline constexpr float ShadowColor[4]{0.05f, 0.05f, 0.05f, 1.0f};
inline constexpr float TextColor[4]{0.85f, 0.85f, 0.85f, 1.0f};

/// Copies that carry a package stamp show it on screen: "FRITCHII V6 TEST" and the copy's id,
/// inside the game picture (`screen`), moving to another spot every minute. Empty for plain
/// builds. `frame_width`/`frame_height` clamp the blocks.
[[nodiscard]] const Blocks& Build(const Common::Rectangle<u32>& screen, u32 frame_width,
                                  u32 frame_height);

} // namespace VideoCore::TestWatermark
