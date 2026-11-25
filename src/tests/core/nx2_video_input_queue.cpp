// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_video_frame_queue.h"
#include "core/hle/service/nx2_video_input_queue.h"

TEST_CASE("NX2 video retains packets submitted beyond the readiness watermark", "[nx2_video_input]") {
    Service::Nx2VideoInputQueue queue;
    constexpr u64 packets = 24;
    for (u64 index = 0; index < packets; ++index) {
        CAPTURE(index);
        queue.Push({{static_cast<u8>(index), 0x82, 0x49, 0x83}, index * 16667,
                    index == packets - 1});
        REQUIRE(queue.Size() == index + 1);
        REQUIRE(queue.BelowReadyWatermark() == (index + 1 < queue.ReadyWatermark));
    }
    for (u64 index = 0; index < packets; ++index) {
        const auto input = queue.Pop();
        REQUIRE(input.data == std::vector<u8>{static_cast<u8>(index), 0x82, 0x49, 0x83});
        REQUIRE(input.pts == index * 16667);
        REQUIRE(input.end == (index == packets - 1));
        REQUIRE(queue.BelowReadyWatermark() == (packets - index - 1 < queue.ReadyWatermark));
    }
    REQUIRE(queue.Empty());
}

TEST_CASE("NX2 video queues preserve independent interleaved sessions", "[nx2_video_input]") {
    std::array<Service::Nx2VideoInputQueue, 3> queues;
    for (u64 frame = 0; frame < 32; ++frame) {
        for (u8 session = 0; session < queues.size(); ++session) {
            queues[session].Push({{session, static_cast<u8>(frame)}, frame * 17000 + session, false});
        }
    }
    for (u64 frame = 0; frame < 32; ++frame) {
        for (u8 session = 0; session < queues.size(); ++session) {
            REQUIRE_FALSE(queues[session].Empty());
            auto input = queues[session].Pop();
            REQUIRE(input.data == std::vector<u8>{session, static_cast<u8>(frame)});
            REQUIRE(input.pts == frame * 17000 + session);
        }
    }
    for (auto& queue : queues) {
        REQUIRE(queue.Empty());
        queue.Push({{0x11}, 42, true});
        queue.Clear();
        REQUIRE(queue.Empty());
        REQUIRE(queue.BelowReadyWatermark());
    }
}

TEST_CASE("NX2 video rejects stale and excess decoded frames", "[nx2_video_delivery]") {
    Service::Nx2VideoFrameQueue queue;
    const u64 first_stream = queue.Generation();
    for (u64 i = 0; i < queue.Capacity; ++i) {
        REQUIRE(queue.Push({{}, i, false, i}, first_stream));
    }
    CHECK_FALSE(queue.Push({{}, 8, false, 8}, first_stream));
    CHECK(queue.Pop().pts == 0);
    CHECK(queue.Push({{}, 8, false, 8}, first_stream));
    queue.Reset();
    CHECK_FALSE(queue.Push({{}, 9, true, 9}, first_stream));
    CHECK(queue.Empty());
    CHECK(queue.Push({{}, 0, false, 10}, queue.Generation()));
}

TEST_CASE("NX2 video presents the last picture before an empty EOS", "[nx2_video_delivery]") {
    Service::Nx2VideoFrameQueue queue;
    const Tegra::Host1x::Nx2Picture picture{};
    REQUIRE(queue.Push({picture, 100, false, 1}, queue.Generation()));
    REQUIRE(queue.Push({picture, 200, false, 2}, queue.Generation()));
    REQUIRE(queue.Push({{}, 300, true, 3}, queue.Generation()));
    CHECK(queue.DropOvertaken(300, true) == 1);
    const auto last = queue.Pop();
    CHECK(last.pts == 200);
    CHECK(last.picture.has_value());
    CHECK(queue.Pop().end);

    REQUIRE(queue.Push({picture, 400, false, 4}, queue.Generation()));
    queue.MarkEnd(500, 5);
    REQUIRE(queue.Push({picture, 0, false, 6}, queue.Generation()));
    CHECK(queue.DropOvertaken(1'000, true) == 0);
    const auto end = queue.Pop();
    CHECK(end.end);
    CHECK(end.pts == 400);
    CHECK(end.submission == 5);
    CHECK(queue.Pop().submission == 6);
}
