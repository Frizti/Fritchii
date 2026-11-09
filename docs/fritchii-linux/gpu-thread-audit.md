# GPU thread CPU bottleneck: audit and profiling plan (Linux, RDNA2, RADV)

Branch `linux-v5` (Fritchii v5 + Linux/AMD changes). Target: Ryzen 5 5500 (6c/12t, one 16 MiB L3)
+ RX 6900 XT (RADV, Mesa 26.2.6), SteamOS, The Legend of Zelda: Ocarina of Time (0400486024DEC000).

## 1. Architecture

The guest's GPU work runs on two host threads, split at method calls (`NX2_GPU_THREAD_SPLIT.md`,
`EDEN_NX2_SPLIT=2`, on by default):

| Stage | Thread | Main code |
|---|---|---|
| GPFIFO entries, command lists | front (`GPU`) | `dma_pusher.cpp` `DmaPusher::Step` (210), `DispatchCalls` (174) |
| Puller methods, semaphore acquires | front | `engines/puller.cpp` `Puller::CallMethod` (1603), `ReleaseFencesForReadable` (801) |
| 3D/compute macros (JIT) against register mirrors | front | `macro/`, `macro_turing_jit.h` |
| Macro DMA reads (MME_DMA_READ) | front, waits for writers | `engines/maxwell_3d.cpp` `Maxwell3D::FrontDmaRead` (2058) |
| Front -> back ring (SPSC of engine/method/argument) | both | `gpu_split.h` `class GpuSplit` (99), `gpu_split.cpp` back loop (147), `Drain` (314) |
| Engine methods, register state, dirty flags | back (`GPUBack`) | `engines/maxwell_3d.cpp` `Maxwell3D::CallMethod` (521) |
| Draw setup | back | `renderer_vulkan/vk_rasterizer.cpp` `PrepareDraw` (435), `Draw` (780) |
| Pipeline lookup, descriptor setup | back | `vk_pipeline_cache.cpp` `CurrentGraphicsPipeline` (2226), `vk_graphics_pipeline.cpp` `ConfigureImpl` (859) |
| Buffer binding and uploads | back | `buffer_cache/buffer_cache.h` `UpdateGraphicsBuffers` (770), `BindHostStageBuffers` (806), `BindHostGraphicsUniformBuffer` (1610), `FindBuffer` (3007) |
| Vulkan command recording/submission | back records; `VulkanWorker` executes | `vk_scheduler.cpp` `Flush` (168), `WorkerThread` (532), `SubmitExecution` (814) |
| Presentation, frame pacing, frame generation spacing | `VulkanPresent` | `vk_present_manager.cpp` `PresentThread` (360), `vk_host_pacer.cpp` |
| Shader translation of skippable draws | 2 translation workers (`EDEN_NX2_ASYNC_TRANSLATE=1`) | `vk_pipeline_cache.cpp` `TranslateOnWorker`, `vk_pipeline_translate_queue.*` |
| Pipeline builds, warm-up, prebuild, preparation | pipeline workers, `VkPrecompile` | `vk_pipeline_cache.cpp` `PlacePrebuildThread`, `PlacePrecompileThread`, `nx2_precompile.cpp` |

Sync points where the front waits for the back: unmet semaphore acquires (cross-channel QMD
releases), macro DMA reads of memory the back still writes, syncpoint waits, WaitForIdle.

## 2. Existing optimizations (do not duplicate)

- Split: 1.46x throughput on title 1 when introduced (NX2_NOTES row 145).
- `Nx2Perf` switches (`nx2_perf.h`): method fast dispatch, cb_batch, usage_tracker, bank_check,
  frames_ahead, pipeline_retention, write_intersections, sorted_page_batches, unswizzle_workers
  on; clean_words, bounds, index_rebind, async_pipelines on here through Graphics > Advanced;
  front_sleep on here by default; submit_budget, fence_flush, fragment_barrier off.
- Kept experiments on by default (`nx2_debug_control.h` `LoadExperiments`): EXP8, EXP9 (native
  bank pages compared only when they may have changed), EXP10 (macro DMA reads wait only for their
  writers), EXP14, EXP21.
- v5: async shader translation, shader preparation (recipes), image pool + render target
  relocation (EXP42/43), graphics pipeline libraries with fast linking (RADV included), parts of
  zortos293/Lichii#805 (command list waits, publish to a waiting back, entry waits).
- Linux additions on this branch: thread pinning from sysfs topology (`common/thread.cpp`),
  optional exclusive GPU cores, background builds kept off the GPU cores
  (`BackgroundWorkCpuMask`), shader preparation on RADV, occlusion query copies wait on the last
  query on RADV, frame generation presents one per refresh.

## 3. Profiling facilities

- `EDEN_NX2_GPU_PROFILE=1`: per thread (`GPU thread`, `GPU back thread`), every 10 s, ~50 zones'
  exclusive ms per second and calls per second (`nx2_profile.h`): method dispatch, macros, draw
  setup and its parts (pipeline lookup, configure, descriptors, uniform/geometry/storage buffers,
  bind stage buffers, push descriptors, render targets, dynamic state, draw record), compute,
  texture upload, fences, split drain/wait, writer and list waits, query DMA waits, cbuf
  bind/upload, queries; host CPU busy ms per second; acquire and DMA read counters.
- `EDEN_NX2_GPU_PROFILE=2`: fine zones and counters (uniform rebinds/streamed/cached, storage
  binds, texture handles).
- `Host pacing` report every 10 s: shown frames, refreshes held, same-refresh drops, interval
  mean/max, present-to-shown latency, game frame time.
- `Fence writer waits`, `Native bank check`, `DMA snapshot` reports every 5 s.
- Added on this branch (this audit): the 10 longest single calls per zone (`... longest calls:`,
  zones reaching 1 ms) in the GPU profile, and frame-time p50/p95/p99 (`Host pacing frame
  time: ...`).
- Not covered: lock contention (no mutex is instrumented), Vulkan submission time on the worker
  as a zone of its own (the back's `FlushWork` covers the hand-off only).

## 4. Measurements so far (RX 6900 XT, Ryzen 5 5500, 60 FPS mode, gameplay)

| Source | Result |
|---|---|
| GPU profile, v4.1-based build | back thread 755-858 ms/s busy: method 130-160, bind stage buffers 120-150, other ~100, compute 67-75, update buffers ~45, draw ~45, fence ~46 ms/s. Front: split drain 450-500 ms/s, macro 140-190, resume pending 60-160 |
| v5 log (no profile) | game frame 16.7-19.8 ms (50-60 fps); front acquire full drains 1,000-2,450/s, 185-356 ms/s; 1.7-2.0 s hitches from in-play shader builds (preparation skipped on RADV, fixed since) |
| Presentation (frame generation on) | 88-95% of frames shown for one refresh after the present spacing fixes |

Conclusion so far: the limit is the back thread's CPU time; the front's acquire drains add to it
where they leave the back idle. No v5 zone profile exists yet, so the split of the back's time on
v5 (with v5's image pool, async translation and the fast buffer binding switches) is unknown.

### v5 profile (2026-11-08, build 49531e3b63, `EDEN_NX2_GPU_PROFILE=2`, 60 FPS mode, LSFG 3x)

Game frame 18-21 ms (48-56 fps), ~80,000 draws/s. Shader preparation did not run (no recipe in
`~/Downloads/bin/recipes`), so 0.57-1.0 s hitches remain in two windows.

- Back: 917 ms/s CPU busy (~11 us per draw). method 113 ms (3.35 M/s; 2.05 M 3D register writes,
  1.31 M same value), other 106, split wait 106-227 (idle for the front), uniform buffers 85
  (165 k/s; cb upload 11, cb bind 7), compute 64, fence 56 (10 k/s), pipeline lookup 43, query
  report 38 (69 k/s), update buffers 34, bind stage buffers 35, geometry buffers 22. Fine
  counters: uniform rebinds ~250 k/s, streamed ~140 k/s, cached ~75 k/s.
- Front: 662 ms/s busy. split drain 487 ms (7,467/s): fence release 253 ms (1,638/s), macro DMA
  read 218 ms (2,875/s), macro DMA flush 13, list writers 5, acquire 3. macro 226 (1.2 M/s), push
  198.

Reading: the threads alternate. The front drains the back ~7,500 times a second; the back then
idles 106-227 ms/s waiting for the next methods. That idle time bounds what removing front drains
can give (~12-15%).

Ranking: (1) fence release drains -> #805 semaphore view; (2) macro DMA read drains -> #805 back
macros (`a442c38fe6`); (3) back uniform buffer handling (~100 ms/s); (4) same-value register
writes (many are trigger methods: draw begin/end 0x585/0x586, report 0x6c3; not droppable).

## 5. Profiling plan

One run per configuration, same save, same route:

1. Load the save at Kakariko Village (heavy, CPU-bound), stand 20 s, walk the main street 40 s,
   pan the camera 20 s. Then Hyrule Field from the Kakariko exit, 60 s. 60 FPS mode, frame
   generation off (it hides the game's own frame times), Occlusion Off off.
2. Launch with `EDEN_NX2_GPU_PROFILE=2 MESA_SHADER_CACHE_MAX_SIZE=10G`.
3. Read from the log per 10 s window: game frame and p50/p95/p99 (`Host pacing`), back and front
   ms/s per zone, longest calls, host CPU busy, acquire and DMA read counters, fine counters
   (uniform/storage binds per second), draws per second (`draw` calls/s).
4. Derived: back CPU time per draw = back busy ms/s / draws per s; sync wait = front split drain +
   writer wait + list waits.

Each optimization is then measured against this baseline on the same route, one change at a
time, behind its own switch, and reported as the CPU time of the zone it targets, not by fps
alone.

## 6. Candidates, to rank once the v5 profile is in

| Candidate | Targets | Risk |
|---|---|---|
| Semaphore view (zortos293/Lichii#805 `fb4d4e47a7`, `188baa1448`, `87a44ccece`, `802c5e4592`, `894a9e8386`, with `EDEN_NX2_DRAIN_CHECK`) | front acquire drains (185-356 ms/s) | high: sync, conflicts with Fritchii's acquire code |
| #805 upload batching (`d612710df3`), uploads ahead of work (`ddf01f582a`) | back recording, render pass breaks | medium |
| #805 back macros (`a442c38fe6`, `1d9b6ef31d`) | front waits on macro DMA reads | high |
| Whatever the v5 fine profile shows as the largest back zone | back CPU time | depends |
