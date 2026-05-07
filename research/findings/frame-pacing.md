# Frame pacing & FPS limiter

How the game's main loop is structured, where the per-frame tick lives, and how to insert an FPS cap cleanly.

## Main loop architecture

`game_MessageLoop` (`0x0056F6E0`) is the game's `WinMain` body. Standard `CWinApp::Run`-style:

```c
WPARAM __stdcall game_MessageLoop(int hInstance, int hPrevInstance, int lpCmdLine, int nShowCmd) {
    SetThreadAffinityMask(GetCurrentThread(), 1);   // pin to CPU 0
    if (!g_App_singleton[0]) return 0;
    g_App_singleton[2] = hInstance;
    if (!App->vtable[1](App, hPrev, cmdLine, nShow)) return 0;   // App->InitInstance
    while (1) {
        while (!PeekMessageA(&msg, 0, 0, 0, PM_NOREMOVE)) {
            App->vtable[4](App);                                  // <-- per-frame idle tick
        }
        if (!GetMessageA(&msg, 0, 0, 0)) break;
        if (!App->vtable[3](App, &msg))                           // App->PreTranslateMessage
            { TranslateMessage(&msg); DispatchMessageA(&msg); }
    }
    App->vtable[2](App);    // App->ExitInstance
    App->vtable[7](App);    // App->Destruct
    return msg.wParam;
}
```

Called from the C runtime startup at `0x0062B5C0` (the standard `mainCRTStartup` shim that calls `WinMain(hInstance, hPrev, cmdLine, nShow)`). This call site IS visible in the dump because the CRT runs at process load.

`g_App_singleton` is at `0x0072F710`. `g_App_singleton[0]` = pointer to the App instance, vtable starts at `*g_App_singleton[0]`. Per-frame idle handler is at vtable[4] (byte offset 16). This is where the game does its update + submit-to-renderer work each frame.

`SetThreadAffinityMask(_, 1)` pins the main thread to CPU 0 — period-of-time hack from the SecuROM era to avoid per-core RDTSC drift. Not relevant for our limiter design.

## Engine clock

`game_get_time_ms` (`0x004A3CCE`) is the engine's millisecond clock:

```c
int game_get_time_ms() {
    if (g_time_cache_valid) return g_cached_time_ms;
    if (!Frequency.QuadPart) game_init_perf_timer();
    QueryPerformanceCounter(&now);
    return MulDiv(now - base_qpc, 1000, Frequency);
}
```

30+ functions in the engine call it via the thunk at `sub_584710 → game_get_time_ms`. Animation, AI, particles, audio sync — all go through it. **The game's update is time-based, not frame-tick-based**, which means an FPS cap should not change game speed. (Caveat: validate empirically — old games sometimes have tick-based bits hidden in unexpected places.)

`game_init_perf_timer` (`0x00584670`) initialises the QPC base and `Frequency` once at startup, plus 16 "last frame timestamp" slots at `0x00741330..0x0074136C`.

### CAVEAT: physics is frame-rate dependent (engine limitation)

Discovered 2026-05-06 during the systems-survey RE: the **physics state
machine** (`physics_state_machine_tick` @ `0x4DC150`) integrates with a
**hardcoded 0.003 sec step** (= 333Hz). Example:
```c
*(float *)(v5 + 36) = 0.003 * *(float *)(v5 + 44) + *(float *)(v5 + 36);
//   pos          +=   0.003 * vel
```
This Euler step is NOT scaled by frame dt. So at higher render FPS,
physics advances faster (each rendered frame = one 0.003-sec tick).
Animation and AI use `game_get_time_ms` and stay correct, but anything
physics-driven (jump arcs, projectile motion, the `physics_state_machine_tick`
state machine) **changes speed with frame rate**.

**Implication for the FPS limiter:** Test 8 (cap 30 vs 144) is *expected*
to show jump-height / velocity differences. This is not a bug in the
limiter — it's the original engine's design. The limiter still gives
clean caps at any target rate; just be aware the game wasn't authored
for arbitrary frame rates.

The `flt_6FFCBC` global at `0x6FFCBC` holds the value `0.003f` — same
constant, sometimes referenced symbolically. See
[systems-survey-2026-05-06.md](systems-survey-2026-05-06.md) for the
full simulation-tick map.

### Decoupling render from sim (the Bloodborne approach)

If you actually want **240 Hz render without breaking the simulation**,
the answer is to decouple the two: throttle simulation to 60 Hz while
letting render run at any rate, then interpolate world state per render
frame. See [`high-fps-decoupling.md`](high-fps-decoupling.md) for the
full architectural plan, hook points, and Phase 1 / Phase 2 scope
estimates.

## FPS limiter design

### Hook point

Insert the limiter in `hk_EndScene` — we already hook this for ImGui draw, so the cost is zero new hooks. `EndScene` is the natural per-frame boundary in D3D9. Sleeping there caps the visible frame rate without affecting the simulation tick (which has already run by then).

### Sleep strategy

```cpp
HRESULT WINAPI hk_EndScene(IDirect3DDevice9* dev) {
    // ... existing ImGui draw + screenshot capture ...
    HRESULT r = g_orig_EndScene(dev);

    if (mtr::fps_limit::enabled()) {
        static uint64_t qpc_freq = 0;
        static uint64_t last_qpc = 0;
        if (!qpc_freq) QueryPerformanceFrequency((LARGE_INTEGER*)&qpc_freq);

        const int fps = mtr::fps_limit::current();
        const uint64_t target_dt = qpc_freq / (uint64_t)fps;
        uint64_t now;
        for (;;) {
            QueryPerformanceCounter((LARGE_INTEGER*)&now);
            const uint64_t elapsed = now - last_qpc;
            if (elapsed >= target_dt) break;
            const uint64_t remaining_us = (target_dt - elapsed) * 1'000'000ULL / qpc_freq;
            if (remaining_us > 1500)      Sleep(1);
            else if (remaining_us > 200)  SwitchToThread();
            // else busy-spin to next QPC sample
        }
        last_qpc = now;
    }
    return r;
}
```

Mixed strategy:
- Coarse sleeps (`Sleep(1)`) when more than ~1.5 ms remain — gives back the CPU.
- `SwitchToThread()` when 200 µs–1.5 ms remain — yields without blocking.
- Tight QPC-spin when under 200 µs — pixel-accurate cap without timer-resolution noise.

Don't call `timeBeginPeriod(1)` — Windows scheduler timer resolution is global, and modern Windows already runs at 1 ms when a recent foreground app needs it. `Sleep(1)` typically returns within 1–2 ms on modern hardware without any explicit `timeBeginPeriod` call.

### State

```cpp
namespace mtr::fps_limit {
    std::atomic<int>  g_target  {0};      // 0 = off, otherwise target fps
    bool enabled()         { return g_target.load() > 0; }
    int  current()         { return g_target.load(); }
    void set(int fps)      { g_target.store(fps); }
}
```

Menu UI in `menu.cpp`:
- Checkbox "Limit FPS"
- Combo: 30 / 60 / 120 / 144 / 240 / Custom
- Slider for custom value (15–500)

### Per-monitor refresh-rate auto-detect (optional)

```cpp
DEVMODE dm = {};
dm.dmSize = sizeof(dm);
EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
const int monitor_hz = dm.dmDisplayFrequency;   // e.g. 60, 144, 240
```

Default the cap to monitor refresh rate when the user enables it.

### Validation

1. Set cap to 30. Walk in-game; animations should look fluid but framerate-locked. No game-speed change.
2. Set cap to 144 (or monitor max). Should match monitor refresh.
3. Disable. Should free-run (typically several hundred fps on modern hardware, since the engine is from 2007).
4. **Crucial test**: set cap to 30, then to 144 — character animation speed, jumping height, walking speed must be identical. Any change indicates a frame-tick-based path we missed; back off to dxwrapper's `LimitPerFrameFPS` with a known-good value.

### Fallback

If the in-mod limiter introduces stutter or a game-logic regression, fall back to dxwrapper's blunt cap: in `Game/dxwrapper.ini`:

```ini
[d3d9]
LimitPerFrameFPS = 60
```

This caps at the swap-chain level (similar mechanism, slightly less control).

## Open questions

- Is App's vtable[4] purely "tick + present" or does it gate on a maximum tick frequency? If it self-throttles (e.g. via `Sleep` when below target frame time), our cap might compound. Inspect the singleton at runtime to verify — ImGui menu could expose `g_App_singleton[0]` content.
- Is there hidden tick-based logic? Animation timestamps are time-driven (`game_get_time_ms`) but particle systems, fluid sims, or scripted events may not be.

## Files & symbols

| VA           | Symbol                                | Role |
|--------------|---------------------------------------|------|
| `0x0056F6E0` | `game_MessageLoop`                    | WinMain. Standard message pump. |
| `0x0056F757` | (call site)                           | `App->vtable[4]()` per-frame idle tick. |
| `0x0062B5C0` | _CRT startup_                         | Calls `game_MessageLoop` after `GetStartupInfoA`. |
| `0x0072F710` | `g_App_singleton`                     | App pointer at `[0]`, hInstance at `[2]`. |
| `0x004A3CCE` | `game_get_time_ms`                    | Engine ms clock (QPC-based). |
| `0x00584670` | `game_init_perf_timer`                | One-time QPC base + frequency setup. |
| `0x00584710` | `(thunk to game_get_time_ms)`         | Used by 30+ engine callers. |
| `0x00741310..0x00741378` | _timer state_             | QPC base, frequency, 16 last-frame stamps. |
