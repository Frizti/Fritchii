# [Fritchii v5.5][Ocarina of Time] Emulated program panics, unmapped memory reads, failed pipeline compilation and slow shutdown

## Environment
- Emulator: Fritchii v5.5, build `oot-gather-fix-4012e5271e` (2026-11-09)
- Game being tested: The Legend of Zelda: Ocarina of Time (user-reported)
- OS: Windows 11 25H2 (build 26200.9711)
- CPU reported by export: AMD Ryzen 5 5600GT (6 cores/12 threads)
- RAM: 15.9 GiB
- GPU: AMD Radeon RX 7600, driver 26.10.1
- Backend: Vulkan; docked mode, 1x resolution, GPU accuracy Low, async shaders enabled
- OoT 30 FPS toggle: false
- Environment toggles: see attached system-info.txt

## Observed behavior
The attached sessions show multiple emulated-program panics, unmapped device/GPU memory reads, pipeline compilation failures, and trouble stopping the emulation thread. The player reports problems while playing Ocarina of Time. The exact visible symptom, reproduction steps, and whether specific log events coincide with a freeze or crash have not been independently confirmed.

## Key evidence (timestamps relative to emulator session)
### Current session (`log-this-session.txt`)
- 1311.185s — Critical: unknown/unimplemented service function `903`, port `nx2:gsv` (line 2871).
- 1326.759s — 16 consecutive `Unmapped Device ReadBlock` errors (lines 2978–2993).
- 1330.907s — Critical: unimplemented function `10500`, port `prepo:u` (line 3102).
- 1395.538s — `Userspace PANIC!` followed by `Emulated program broke execution!` and ARM backtrace (lines 4255–4258).
- 3771.999–3774.397s — 16 `Read64 of unmapped GPU address` errors (lines 23124–23181).
- 3789.643s — Second `Userspace PANIC!` and emulated execution break (lines 23285–23288).
- 3791.392s — Pipeline compilation summary: 35,085 prepared, 25 failed, 85,351 not reached (line near end).
- 3799.636s — Emulation thread still shutting down after 9,999ms; cleanup eventually completed at 3808.987s.

### Previous session (`log-previous-session.txt`)
- Pipeline summary: 90,071 prepared, 28 failed, 30,362 not reached (near end).
- 3040.639s — `Emulation thread failed to stop, terminating it` (last line).

## Questions / request for investigation
- Could the `nx2:gsv` service assert, memory mapping errors, or other emulation behavior explain the two userspace panics?
- Are pipeline failures and unfinished background pipeline preparation expected in this build?
- Is the stalled emulation shutdown a known issue?

## Reproduction steps
Not established from logs alone. Please ask the reporter for the exact in-game location, action, and symptom. The logs and system information are attached for triage.

## Attached files
- `system-info.txt` — settings and system details
- `log-this-session.txt` — complete current-session trace
- `log-previous-session.txt` — complete previous-session trace
- `error-excerpts.txt` — short verbatim examples with original line numbers

**Privacy:** The export already anonymizes Windows user/PC names as `user`/`PC`. Please review attachments before public posting. No emulation logs were altered to fabricate or change error events.
