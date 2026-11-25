// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

#include "common/common_types.h"
#include "video_core/host1x/nx2_video_decoder.h"

namespace Service {

// The session mutex protects the queue and its generation. A decode from before Reset
// may finish afterwards, but must not publish frames to the new stream.
class Nx2VideoFrameQueue {
public:
    struct Frame {
        std::optional<Tegra::Host1x::Nx2Picture> picture;
        u64 pts;
        bool end;
        u64 submission;
    };

    static constexpr size_t Capacity = 8;

    u64 Generation() const {
        return generation;
    }
    bool Empty() const {
        return frames.empty();
    }
    size_t Size() const {
        return frames.size();
    }
    const Frame& Front() const {
        return frames.front();
    }
    bool HasRoom(size_t capacity = Capacity) const {
        return frames.size() < capacity;
    }

    void Reset() {
        frames.clear();
        ++generation;
    }

    bool Push(Frame frame, u64 expected_generation, size_t capacity = Capacity) {
        if (expected_generation != generation || !HasRoom(capacity)) {
            return false;
        }
        frames.push_back(std::move(frame));
        return true;
    }

    Frame Pop() {
        auto frame = std::move(frames.front());
        frames.pop_front();
        return frame;
    }

    void MarkEnd(u64 pts, u64 submission) {
        if (frames.empty()) {
            frames.push_back({std::nullopt, pts, true, submission});
        } else {
            frames.back().end = true;
            frames.back().submission = submission;
        }
    }

    // A standalone EOS has no picture to show. In the parallel path, only an actual later
    // picture can supersede a late picture.
    u32 DropOvertaken(s64 due, bool parallel) {
        u32 dropped{};
        while (frames.size() >= 2) {
            const auto& front = frames[0];
            const auto& next = frames[1];
            if (front.end || static_cast<s64>(next.pts) > due ||
                (parallel && (!next.picture || next.pts <= front.pts))) {
                break;
            }
            frames.pop_front();
            ++dropped;
        }
        return dropped;
    }

private:
    std::deque<Frame> frames;
    u64 generation{};
};

} // namespace Service
