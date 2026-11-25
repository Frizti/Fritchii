// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <optional>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/nx2_video_input_queue.h"
#include "core/hle/service/nx2_video_ready.h"
#include "tests/core/system_test_context.h"

TEST_CASE("Video readiness follows the queue watermark and decoder state", "[nx2][video]") {
    ScopedSystemTestContext environment;
    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();
    std::optional<Kernel::KThread> thread{std::in_place, kernel};
    REQUIRE(Kernel::KThread::InitializeDummyThread(system, &*thread, nullptr).IsSuccess());
    kernel.SetCurrentEmuThread(&*thread);
    kernel.Initialize();
    {
        Service::KernelHelpers::ServiceContext context{system, "VideoReadyTest"};
        auto* event = context.CreateEvent("VideoReadyTest:Ready");
        const auto is_signaled = [&] {
            Kernel::KScopedSchedulerLock lock{kernel};
            return event->GetReadableEvent().IsSignaled(kernel);
        };
        Service::Nx2VideoReadyState ready;
        ready.event = event;
        ready.Signal(kernel);
        CHECK(is_signaled());

        ready.SetRunning(kernel, false);
        CHECK_FALSE(is_signaled());
        ready.room.store(false);
        ready.room.store(true);
        ready.Signal(kernel);
        CHECK_FALSE(is_signaled());

        ready.SetRunning(kernel, true);
        ready.Signal(kernel);
        CHECK(is_signaled());
        event->Clear(kernel);
        ready.room.store(false);
        ready.Signal(kernel);
        CHECK_FALSE(is_signaled());
        ready.room.store(true);
        ready.Signal(kernel);
        CHECK(is_signaled());

        // Packets above the watermark are still accepted. Only crossing below it wakes
        // a submitter, and the decoder thread can do so without a timing callback.
        Service::Nx2VideoInputQueue inputs;
        for (size_t i = 0; i <= inputs.ReadyWatermark; ++i) {
            inputs.Push({{}, i, false});
        }
        event->Clear(kernel);
        ready.room.store(inputs.BelowReadyWatermark());
        inputs.Pop();
        CHECK_FALSE(ready.UpdateRoom(inputs.BelowReadyWatermark()));
        inputs.Pop();
        REQUIRE(ready.UpdateRoom(inputs.BelowReadyWatermark()));
        std::thread worker{[&] {
            system.RegisterHostThread();
            ready.Signal(kernel);
        }};
        worker.join();
        CHECK(is_signaled());

        event->Clear(kernel);
        inputs.Pop();
        CHECK_FALSE(ready.UpdateRoom(inputs.BelowReadyWatermark()));
        CHECK_FALSE(is_signaled());

        // Refill or Stop can invalidate an edge before it reaches the event.
        inputs.Push({{}, 1, false});
        inputs.Push({{}, 2, false});
        CHECK_FALSE(ready.UpdateRoom(inputs.BelowReadyWatermark()));
        ready.Signal(kernel);
        CHECK_FALSE(is_signaled());
        inputs.Pop();
        REQUIRE(ready.UpdateRoom(inputs.BelowReadyWatermark()));
        ready.SetRunning(kernel, false);
        ready.Signal(kernel);
        CHECK_FALSE(is_signaled());
        ready.event = nullptr;
        context.CloseEvent(event);
        ready.Signal(kernel);
    }
    system.CoreTiming().ClearPendingEvents();
    kernel.Shutdown();
    kernel.SetCurrentEmuThread(nullptr);
    kernel.UnregisterKernelObject(&*thread);
    thread.reset();
}
