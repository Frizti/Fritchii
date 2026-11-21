// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <thread>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/nx2_timeline.h"
#include "video_core/host1x/syncpoint_manager.h"

#include <cstdlib>
#include <cstring>

namespace Tegra {

namespace Host1x {

namespace {
/// FRITCHII_SYNCPT_UNLOCKED (boot, default 1; 0 = actions run under the lock, as before).
bool ActionsUnlocked() {
    static const bool unlocked = [] {
        const char* value = std::getenv("FRITCHII_SYNCPT_UNLOCKED");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    return unlocked;
}
} // namespace

void SyncpointManager::RunDueBatches(std::unique_lock<std::mutex>& lk) {
    // However the loop ends (an action may throw), the runner's state is cleared under the lock
    // and the threads waiting on it are told.
    struct EndRun {
        SyncpointManager& manager;
        std::unique_lock<std::mutex>& lk;
        ~EndRun() {
            if (!lk.owns_lock()) {
                lk.lock();
            }
            manager.running_ids.clear();
            manager.actions_running = false;
            manager.actions_runner = {};
            manager.running_cv.notify_all();
        }
    } end_run{*this, lk};
    while (!due_batches.empty()) {
        DueBatch batch = std::move(due_batches.front());
        due_batches.pop_front();
        running_ids.clear();
        for (const RegisteredAction& action : batch.actions) {
            running_ids.push_back(action.id);
        }
        lk.unlock();
        for (RegisteredAction& action : batch.actions) {
            action.action();
        }
        lk.lock();
        running_ids.clear();
        batch.wait_cv->notify_all();
        running_cv.notify_all();
    }
}

bool SyncpointManager::ActionDue(u64 id) const {
    if (std::ranges::find(running_ids, id) != running_ids.end()) {
        return true;
    }
    return std::ranges::any_of(due_batches, [id](const DueBatch& batch) {
        return std::ranges::any_of(batch.actions,
                                   [id](const RegisteredAction& action) { return action.id == id; });
    });
}

SyncpointManager::ActionHandle SyncpointManager::RegisterAction(
    std::atomic<u32>& syncpoint, std::list<RegisteredAction>& action_storage, u32 expected_value,
    std::function<void()>&& action) {
    if (syncpoint.load(std::memory_order_acquire) >= expected_value) {
        action();
        return {};
    }

    std::unique_lock lk(guard);
    if (syncpoint.load(std::memory_order_relaxed) >= expected_value) {
        if (ActionsUnlocked()) {
            lk.unlock();
        }
        action();
        return {};
    }
    auto it = action_storage.begin();
    while (it != action_storage.end()) {
        if (it->expected_value >= expected_value) {
            break;
        }
        ++it;
    }
    return action_storage.emplace(it, expected_value, ++last_action_id, std::move(action))->id;
}

void SyncpointManager::DeregisterAction(std::list<RegisteredAction>& action_storage,
                                        const ActionHandle& handle) {
    if (handle == 0) {
        return;
    }
    std::unique_lock lk(guard);

    for (auto it = action_storage.begin(); it != action_storage.end(); it++) {
        if (it->id == handle) {
            action_storage.erase(it);
            return;
        }
    }
    // Taken by an increment (queued or running unlocked): return once it has run. On the
    // runner (from inside an action) a queued one is dropped instead, as if this came first:
    // waiting there would wait for itself.
    if (actions_running && actions_runner == std::this_thread::get_id()) {
        for (DueBatch& batch : due_batches) {
            std::erase_if(batch.actions,
                          [&handle](const RegisteredAction& action) { return action.id == handle; });
        }
        return;
    }
    running_cv.wait(lk, [&] { return !ActionDue(handle); });
}

void SyncpointManager::DeregisterGuestAction(u32 syncpoint_id, const ActionHandle& handle) {
    DeregisterAction(guest_action_storage[syncpoint_id], handle);
}

void SyncpointManager::DeregisterHostAction(u32 syncpoint_id, const ActionHandle& handle) {
    DeregisterAction(host_action_storage[syncpoint_id], handle);
}

void SyncpointManager::IncrementGuest(u32 syncpoint_id) {
    if (syncpoint_id >= NUM_MAX_SYNCPOINTS) {
        LOG_ERROR(HW_GPU, "Ignoring increment of invalid syncpoint {:#x}", syncpoint_id);
        return;
    }
    Increment(syncpoints_guest[syncpoint_id], wait_guest_cv, guest_action_storage[syncpoint_id],
              "syncpoint: guest increment", syncpoint_id, true);
    if (const auto hook = guest_increment_hook.load(std::memory_order_acquire)) {
        hook(guest_increment_context.load(std::memory_order_relaxed));
    }
}

void SyncpointManager::SetGuestMirror(const void* owner, GuestMirror mirror) {
    std::scoped_lock lk(guard);
    guest_mirror = std::move(mirror);
    guest_mirror_owner = guest_mirror ? owner : nullptr;
    has_guest_mirror.store(static_cast<bool>(guest_mirror), std::memory_order_relaxed);
}

void SyncpointManager::ClearGuestMirror(const void* owner) {
    std::scoped_lock lk(guard);
    if (guest_mirror_owner != owner) {
        return;
    }
    guest_mirror = {};
    guest_mirror_owner = nullptr;
    has_guest_mirror.store(false, std::memory_order_relaxed);
}

void SyncpointManager::IncrementHost(u32 syncpoint_id) {
    if (syncpoint_id >= NUM_MAX_SYNCPOINTS) {
        return;
    }
    Increment(syncpoints_host[syncpoint_id], wait_host_cv, host_action_storage[syncpoint_id],
              "syncpoint: host increment", syncpoint_id);
}

void SyncpointManager::WaitGuest(u32 syncpoint_id, u32 expected_value) {
    Wait(syncpoints_guest[syncpoint_id], wait_guest_cv, expected_value, syncpoint_id, true);
}

void SyncpointManager::WaitHost(u32 syncpoint_id, u32 expected_value) {
    Wait(syncpoints_host[syncpoint_id], wait_host_cv, expected_value, syncpoint_id, false);
}

void SyncpointManager::Increment(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                                 std::list<RegisteredAction>& action_storage,
                                 const char* timeline_name, u32 syncpoint_id, bool guest) {
    std::unique_lock lk(guard, std::defer_lock);
    u32 new_value;
    if (guest && has_guest_mirror.load(std::memory_order_relaxed)) {
        // Mirrored before the value is published: the lock-free ready checks (RegisterAction,
        // Wait, IsReadyGuest) must not see a value the client's page does not hold yet. Guest
        // increments take the lock first while a mirror is set, so they mirror in order.
        lk.lock();
        new_value = syncpoint.load(std::memory_order_relaxed) + 1;
        if (guest_mirror) {
            guest_mirror(syncpoint_id, new_value);
        }
        new_value = syncpoint.fetch_add(1, std::memory_order_acq_rel) + 1;
    } else {
        new_value = syncpoint.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    // Before the actions it runs: what ends the waits on this value (critical_path.py).
    if (Common::Nx2Timeline::Active()) {
        Common::Nx2Timeline::Instant(timeline_name, {"id", syncpoint_id}, {"value", new_value});
    }

    if (!lk.owns_lock()) {
        lk.lock();
    }
    if (ActionsUnlocked()) {
        auto end = action_storage.begin();
        while (end != action_storage.end() && end->expected_value <= new_value) {
            ++end;
        }
        if (end == action_storage.begin()) {
            wait_cv.notify_all(); // nothing due: no action to run first
            return;
        }
        DueBatch& batch = due_batches.emplace_back();
        batch.actions.splice(batch.actions.end(), action_storage, action_storage.begin(), end);
        batch.wait_cv = &wait_cv;
        if (actions_running) {
            return; // the runner runs them after the ones before, then wakes the waiters
        }
        actions_running = true;
        actions_runner = std::this_thread::get_id();
        RunDueBatches(lk);
        return;
    }
    auto it = action_storage.begin();
    while (it != action_storage.end()) {
        if (it->expected_value > new_value) {
            break;
        }
        it->action();
        it = action_storage.erase(it);
    }
    wait_cv.notify_all();
}

void SyncpointManager::Wait(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                            u32 expected_value, u32 syncpoint_id, bool guest) {
    const auto pred = [&]() { return syncpoint.load(std::memory_order_acquire) >= expected_value; };
    if (pred()) {
        return;
    }

    std::unique_lock lk(guard);
    if (pred()) {
        return;
    }
    // Only a wait that blocks is listed (for the hang report), under the lock it takes anyway.
    const int token = 0;
    blocked_waits.push_back(
        {&token, syncpoint_id, expected_value, guest, std::chrono::steady_clock::now()});
    wait_cv.wait(lk, pred);
    std::erase_if(blocked_waits, [&token](const BlockedWait& wait) { return wait.token == &token; });
}

std::string SyncpointManager::DescribeWaits() {
    std::unique_lock lk(guard, std::defer_lock);
    for (int attempt = 0; !lk.try_lock(); ++attempt) {
        if (attempt >= 200) {
            return "lock held for 200 ms (an increment or a registration in progress)";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const auto now = std::chrono::steady_clock::now();
    std::string text;
    size_t listed = 0;
    for (u32 id = 0; id < NUM_MAX_SYNCPOINTS; ++id) {
        const auto& guest_actions = guest_action_storage[id];
        const auto& host_actions = host_action_storage[id];
        const bool blocked = std::ranges::any_of(
            blocked_waits, [id](const BlockedWait& wait) { return wait.syncpoint_id == id; });
        if (guest_actions.empty() && host_actions.empty() && !blocked) {
            continue;
        }
        if (++listed > 24) {
            text += "; ...";
            break;
        }
        std::string waits;
        const auto add_actions = [&waits](const char* side,
                                          const std::list<RegisteredAction>& actions) {
            if (actions.empty()) {
                return;
            }
            waits += fmt::format("{}{} actions at", waits.empty() ? "" : ", ", side);
            size_t shown = 0;
            for (const RegisteredAction& action : actions) {
                if (++shown > 6) {
                    waits += fmt::format(" ... ({} in all)", actions.size());
                    break;
                }
                waits += fmt::format(" {:#x}", action.expected_value);
            }
        };
        add_actions("guest", guest_actions);
        add_actions("host", host_actions);
        for (const BlockedWait& wait : blocked_waits) {
            if (wait.syncpoint_id == id) {
                waits += fmt::format("{}a thread blocked in Wait{} for {:#x} ({:.1f} s)",
                                     waits.empty() ? "" : ", ", wait.guest ? "Guest" : "Host",
                                     wait.expected_value,
                                     std::chrono::duration<double>(now - wait.since).count());
            }
        }
        text += fmt::format("{}syncpoint {} guest {:#x} host {:#x}: {}", text.empty() ? "" : "; ",
                            id, syncpoints_guest[id].load(std::memory_order_relaxed),
                            syncpoints_host[id].load(std::memory_order_relaxed), waits);
    }
    return text.empty() ? std::string{"none"} : text;
}

} // namespace Host1x

} // namespace Tegra
