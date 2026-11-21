// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <utility>

#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_pipeline_translate_queue.h"

namespace Vulkan {

TranslateQueue::TranslateQueue(size_t thread_count, size_t max_in_flight_, Translate translate_,
                               Retire retire_, std::string name,
                               Common::ThreadPlacement placement)
    : max_in_flight{max_in_flight_}, translate{std::move(translate_)}, retire{std::move(retire_)},
      threads{new Common::ThreadWorker(thread_count, std::move(name), {}, placement)} {}

TranslateQueue::~TranslateQueue() {
    // A translation in progress finishes and stores its job; the queued ones are dropped. At
    // shutdown, on the GPU thread: the pipeline workers still run, so a pipeline here waits for
    // its build (or runs it) as any other pipeline the cache destroys then.
    threads.reset();
    entries.clear();
    abandoned.clear();
}

bool TranslateQueue::Enqueue(std::unique_ptr<TranslationJob> job) {
    const TranslateQueueKey key{job->key, job->occl_kind};
    u64 id{};
    {
        std::scoped_lock lock{mutex};
        if (entries.size() >= max_in_flight || entries.contains(key)) {
            return false;
        }
        id = next_id++;
        Entry& entry = entries[key];
        entry.id = id;
        entry.enqueued = job->enqueued;
        entry.parent_key = job->parent_key;
        entry.job = std::move(job);
        ++runs_pending;
    }
    threads->QueueWork([this, key, id] { Run(key, id); });
    return true;
}

void TranslateQueue::Run(TranslateQueueKey key, u64 id) {
    std::unique_ptr<TranslationJob> job;
    {
        std::scoped_lock lock{mutex};
        const auto it = entries.find(key);
        if (it == entries.end() || it->second.id != id || !it->second.job) {
            --runs_pending; // Abandoned before it ran
            done_condition.notify_all();
            return;
        }
        job = std::move(it->second.job);
        it->second.started = std::chrono::steady_clock::now();
    }
    const auto begin = std::chrono::steady_clock::now();
    try {
        translate(*job);
    } catch (const std::exception& exception) {
        // CreateGraphicsPipeline handles the translation's own errors; this is anything else
        // (out of memory, a driver error building the pipeline object).
        LOG_ERROR(Render_Vulkan, "Async translate: pipeline {:016x} failed on its thread: {}",
                  job->key.Hash(), exception.what());
        // A pipeline made before the failure stays in the job, for the GPU thread to retire.
        job->outcome = TranslationJob::Outcome::NoPipeline;
    } catch (...) {
        LOG_ERROR(Render_Vulkan, "Async translate: pipeline {:016x} failed on its thread",
                  job->key.Hash());
        job->outcome = TranslationJob::Outcome::NoPipeline;
    }
    if (job->outcome == TranslationJob::Outcome::Pending) {
        job->outcome = job->pipeline ? TranslationJob::Outcome::Built
                                     : TranslationJob::Outcome::NoPipeline;
    }
    job->translation = std::chrono::steady_clock::now() - begin;
    std::scoped_lock lock{mutex};
    --runs_pending;
    done_condition.notify_all();
    const auto it = entries.find(key);
    if (it != entries.end() && it->second.id == id) {
        it->second.job = std::move(job);
        it->second.done = true;
        return;
    }
    // Abandoned while it ran (swept, or the GPU thread built it itself): never bound, kept for
    // the GPU thread to retire (RetireAbandoned) instead of destroyed here.
    abandoned.push_back(std::move(job));
}

bool TranslateQueue::WaitIdle(std::chrono::milliseconds cap) {
    std::unique_lock lock{mutex};
    return done_condition.wait_for(lock, cap, [&] { return runs_pending == 0; });
}

bool TranslateQueue::Contains(const GraphicsPipelineCacheKey& key, u8 kind) const {
    std::scoped_lock lock{mutex};
    return entries.contains(TranslateQueueKey{key, kind});
}

std::unique_ptr<TranslationJob> TranslateQueue::Take(const GraphicsPipelineCacheKey& key,
                                                     u8 kind) {
    std::scoped_lock lock{mutex};
    const auto it = entries.find(TranslateQueueKey{key, kind});
    if (it == entries.end() || !it->second.done) {
        return nullptr;
    }
    auto job = std::move(it->second.job);
    entries.erase(it);
    return job;
}

std::unique_ptr<TranslationJob> TranslateQueue::Wait(const GraphicsPipelineCacheKey& key,
                                                     std::chrono::milliseconds cap, u8 kind) {
    const TranslateQueueKey queue_key{key, kind};
    std::unique_lock lock{mutex};
    const bool finished = done_condition.wait_for(lock, cap, [&] {
        const auto it = entries.find(queue_key);
        return it == entries.end() || it->second.done;
    });
    if (!finished) {
        return nullptr;
    }
    const auto it = entries.find(queue_key);
    if (it == entries.end()) {
        return nullptr;
    }
    auto job = std::move(it->second.job);
    entries.erase(it);
    return job;
}

void TranslateQueue::Abandon(const GraphicsPipelineCacheKey& key, u8 kind) {
    std::unique_ptr<TranslationJob> job;
    {
        std::scoped_lock lock{mutex};
        const auto it = entries.find(TranslateQueueKey{key, kind});
        if (it == entries.end()) {
            return;
        }
        job = std::move(it->second.job);
        entries.erase(it);
    }
    if (job) {
        Discard(std::move(job));
    }
}

std::vector<GraphicsPipelineCacheKey> TranslateQueue::Sweep(
    std::chrono::steady_clock::duration age) {
    std::vector<GraphicsPipelineCacheKey> keys;
    for (const Swept& swept : SweepJobs(age)) {
        keys.push_back(swept.key);
    }
    return keys;
}

std::vector<TranslateQueue::Swept> TranslateQueue::SweepJobs(
    std::chrono::steady_clock::duration age, bool from_start) {
    std::vector<Swept> swept;
    std::vector<std::unique_ptr<TranslationJob>> dropped;
    const auto now = std::chrono::steady_clock::now();
    {
        // Picked and abandoned under one lock: a job whose thread finishes meanwhile stays
        // (Run stores it under this lock), never thrown away as wedged.
        std::scoped_lock lock{mutex};
        for (auto it = entries.begin(); it != entries.end();) {
            const Entry& entry = it->second;
            // A finished job is only waiting for its draw to come back; it is not wedged.
            const bool old =
                from_start ? entry.started != std::chrono::steady_clock::time_point{} &&
                                 now - entry.started > age
                           : now - entry.enqueued > age;
            if (entry.done || !old) {
                ++it;
                continue;
            }
            swept.push_back(Swept{it->first.key, it->first.kind, entry.parent_key});
            if (it->second.job) {
                dropped.push_back(std::move(it->second.job)); // queued, not started
            }
            it = entries.erase(it); // one running ends in `abandoned`
        }
    }
    for (auto& job : dropped) {
        Discard(std::move(job));
    }
    return swept;
}

std::vector<std::unique_ptr<TranslationJob>> TranslateQueue::TakeFinished() {
    std::vector<std::unique_ptr<TranslationJob>> jobs;
    std::scoped_lock lock{mutex};
    for (auto it = entries.begin(); it != entries.end();) {
        if (it->second.done) {
            jobs.push_back(std::move(it->second.job));
            it = entries.erase(it);
        } else {
            ++it;
        }
    }
    return jobs;
}

void TranslateQueue::RetireAbandoned() {
    std::vector<std::unique_ptr<TranslationJob>> jobs;
    {
        std::scoped_lock lock{mutex};
        jobs.swap(abandoned);
    }
    for (auto& job : jobs) {
        Discard(std::move(job));
    }
}

void TranslateQueue::Discard(std::unique_ptr<TranslationJob> job) {
    if (job && job->pipeline) {
        retire(std::move(job->pipeline));
    }
}

size_t TranslateQueue::InFlight() const {
    std::scoped_lock lock{mutex};
    return entries.size();
}

} // namespace Vulkan
