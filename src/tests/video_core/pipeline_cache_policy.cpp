// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "video_core/pipeline_cache_policy.h"

using namespace VideoCommon;

TEST_CASE("Graphics pipelines are evicted only past the limit", "[video_core][pipeline_cache]") {
    SECTION("within the limit nothing is evicted, however stale") {
        CHECK(GraphicsPipelinesToEvict(0, 0, 100) == 0);
        CHECK(GraphicsPipelinesToEvict(100, 100, 100) == 0);
        CHECK(GraphicsPipelinesToEvict(50, 50, 100) == 0);
    }
    SECTION("past the limit only the excess is evicted") {
        CHECK(GraphicsPipelinesToEvict(101, 101, 100) == 1);
        CHECK(GraphicsPipelinesToEvict(150, 80, 100) == 50);
    }
    SECTION("pipelines used recently are kept even past the limit") {
        CHECK(GraphicsPipelinesToEvict(150, 20, 100) == 20);
        CHECK(GraphicsPipelinesToEvict(150, 0, 100) == 0);
    }
    SECTION("a zero limit evicts every stale pipeline") {
        CHECK(GraphicsPipelinesToEvict(10, 7, 0) == 7);
    }
}

TEST_CASE("Prebuild retains recent graphics pipelines within the boot budget",
          "[video_core][pipeline_cache]") {
    std::array candidates{
        GraphicsBootCandidate{0, 2},
        GraphicsBootCandidate{2, 0},
        GraphicsBootCandidate{3, 1},
        GraphicsBootCandidate{4, 0},
    };
    CHECK(SelectGraphicsBootCandidates(candidates, 0).empty());
    CHECK(SelectGraphicsBootCandidates(candidates, 4).size() == 4);

    const auto selected = SelectGraphicsBootCandidates(candidates, 2);
    REQUIRE(selected.size() == 2);
    CHECK(selected[0].record_index == 2);
    CHECK(selected[1].record_index == 4);

    const auto tied = SelectGraphicsBootCandidates(candidates, 1);
    REQUIRE(tied.size() == 1);
    CHECK(tied[0].record_index == 2);
}

TEST_CASE("Background graphics builds fit the budget remaining after boot",
          "[video_core][pipeline_cache]") {
    CHECK(GraphicsBackgroundBudget(6000, 2268, 0) == 2268);
    CHECK(GraphicsBackgroundBudget(6000, 2268, 2000) == 268);
    CHECK(GraphicsBackgroundBudget(6000, 2268, 2268) == 0);
    CHECK(GraphicsBackgroundBudget(6000, 2268, 8771) == 0);
    CHECK(GraphicsBackgroundBudget(500, 2268, 0) == 500);
}

TEST_CASE("Vertex attribute mask follows the header's input map", "[video_core][pipeline_cache]") {
    std::array<std::array<bool, 4>, 32> inputs{};
    CHECK(VertexAttributeMask(inputs) == 0);

    inputs[0] = {true, false, false, false};
    inputs[3] = {false, false, false, true};
    inputs[31] = {false, true, true, false};
    CHECK(VertexAttributeMask(inputs) == ((u64{3} << 0) | (u64{3} << 6) | (u64{3} << 62)));

    inputs.fill({true, true, true, true});
    CHECK(VertexAttributeMask(inputs) == ~u64{0});
}

TEST_CASE("Pipeline keys keep only the attribute types active stages read",
          "[video_core][pipeline_cache]") {
    constexpr u64 types = 0x1234'5678'9abc'def0;
    constexpr u64 first_four = 0xff;

    SECTION("a VertexA program keeps every type") {
        const std::array<u64, 6> hashes{1, 2, 0, 0, 0, 3};
        const std::array<u64, 6> masks{0, 0, 0, 0, 0, 0};
        CHECK(KeyedAttributeTypes(types, hashes, masks) == types);
    }
    SECTION("the vertex program's input map masks the types") {
        const std::array<u64, 6> hashes{0, 2, 0, 0, 0, 3};
        const std::array<u64, 6> masks{0, first_four, 0, 0, 0, ~u64{0}};
        CHECK(KeyedAttributeTypes(types, hashes, masks) == (types & first_four));
    }
    SECTION("inactive stages do not mask") {
        const std::array<u64, 6> hashes{0, 2, 0, 0, 0, 0};
        const std::array<u64, 6> masks{0, ~u64{0}, 0, 0, 0, 0};
        CHECK(KeyedAttributeTypes(types, hashes, masks) == types);
    }
    SECTION("task and mesh programs drop every type") {
        const std::array<u64, 6> hashes{0, 7, 0, 8, 0, 3};
        const std::array<u64, 6> masks{0, 0, 0, 0, 0, ~u64{0}};
        CHECK(KeyedAttributeTypes(types, hashes, masks) == 0);
    }
    SECTION("garbage in unread registers no longer changes the key") {
        const std::array<u64, 6> hashes{0, 2, 0, 0, 0, 3};
        const std::array<u64, 6> masks{0, first_four, 0, 0, 0, ~u64{0}};
        const u64 other = (types & first_four) | 0xabcd'0000'0000'0000;
        CHECK(KeyedAttributeTypes(types, hashes, masks) ==
              KeyedAttributeTypes(other, hashes, masks));
    }
    SECTION("masking a stored key again changes nothing") {
        const std::array<u64, 6> hashes{0, 2, 0, 0, 0, 3};
        const std::array<u64, 6> masks{0, first_four, 0, 0, 0, ~u64{0}};
        const u64 once = KeyedAttributeTypes(types, hashes, masks);
        CHECK(KeyedAttributeTypes(once, hashes, masks) == once);
    }
}
