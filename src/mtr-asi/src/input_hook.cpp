// Low-level mouse hook for events DirectInput8-exclusive eats before they
// reach our WndProc subclass. Originally added to plumb the mouse wheel
// through to ImGui (the game grabs DI-exclusive on the wheel system-wide,
// so WM_MOUSEWHEEL never gets dispatched). Phase B extends this to ALL
// mouse events when an mtr-asi window is visible — clicks/moves are also
// eaten by DI-exclusive in gameplay, which is why pick mode worked in the
// main menu (no exclusive grab) but not during gameplay (HUD un-pickable).
//
// Architecture:
//   - WH_MOUSE_LL fires on a dedicated thread before any WndProc dispatch.
//   - When mtr-asi UI is visible, we serialize every mouse event into a
//     queue and return 1 to swallow — so the game's WndProc, ImGui's
//     WndProc subclass, and DirectInput's grabbed mouse stream all stop
//     seeing the event in the same instant. The render thread drains
//     the queue at the top of the menu frame and feeds ImGui via
//     io.AddMouseXxxEvent. Single source of truth, no double-fire.
//   - When UI is hidden, freecam wheel handling is preserved; everything
//     else passes through unchanged.
//
// Installed on a dedicated thread because WH_MOUSE_LL callbacks fire only
// on the installer's thread when it has a message pump. The thread runs a
// trivial GetMessage loop and never exits during normal operation (process
// teardown reclaims everything).

#include <windows.h>
#include <atomic>
#include <mutex>
#include <vector>

namespace mtr::log     { void info(const char* fmt, ...); }
namespace mtr::freecam {
    bool active();
    void accumulate_wheel(int delta);
    void set_ui_visible(bool v);  // not called here, just for namespace clarity
}
namespace mtr::menu    { bool is_visible(); }
namespace mtr::console { bool is_visible(); }

namespace mtr::input_hook {

// Public type — also used by menu.cpp's drain. Raw screen-space; menu
// converts to client coords before forwarding to ImGui.
struct MouseQueueEvent {
    enum Kind : uint8_t { Pos = 0, Button = 1, Wheel = 2 };
    uint8_t kind;
    uint8_t button;       // 0 = L, 1 = R, 2 = M (Button kind only)
    bool    down;         // (Button kind only)
    int16_t wheel_delta;  // (Wheel kind only) — signed multiples of 120
    int32_t screen_x;     // (Pos / Wheel / Button) — at the event time
    int32_t screen_y;
};

namespace {

std::atomic<HHOOK> g_hook{nullptr};

// Wheel accumulator kept for freecam path (when menu is hidden). When
// menu IS visible, wheel events go through the unified mouse queue
// instead — see push_event() below.
std::atomic<int> g_pending_wheel_delta{0};

std::mutex                         g_event_mu;
std::vector<MouseQueueEvent>       g_event_queue;

void push_event(const MouseQueueEvent& ev) {
    std::scoped_lock lk(g_event_mu);
    // Bound the queue to keep memory predictable under input storms.
    // 256 events is ~3 frames at 60 Hz of pathological input — anything
    // older has lost its temporal relevance for ImGui anyway.
    if (g_event_queue.size() >= 256) {
        g_event_queue.erase(g_event_queue.begin(),
                            g_event_queue.begin() + 64);
    }
    g_event_queue.push_back(ev);
}

LRESULT CALLBACK mouse_ll_proc(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode != HC_ACTION || !lp) {
        return CallNextHookEx(nullptr, nCode, wp, lp);
    }
    const MSLLHOOKSTRUCT* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
    const bool ui_visible = mtr::menu::is_visible() || mtr::console::is_visible();

    if (ui_visible) {
        // Capture every mouse event for ImGui. Returning 1 prevents the
        // event from reaching ANY subsequent hook chain or window
        // procedure, including DirectInput's grabbed stream and ImGui's
        // own WndProc subclass — so the queue is the single source of
        // truth, no double-fire, no game-side input bleed-through.
        MouseQueueEvent ev{};
        ev.screen_x = m->pt.x;
        ev.screen_y = m->pt.y;

        switch (wp) {
        case WM_MOUSEMOVE:
            ev.kind = MouseQueueEvent::Pos;
            push_event(ev);
            return 1;
        case WM_LBUTTONDOWN:
            ev.kind = MouseQueueEvent::Button;
            ev.button = 0; ev.down = true;
            push_event(ev);
            return 1;
        case WM_LBUTTONUP:
            ev.kind = MouseQueueEvent::Button;
            ev.button = 0; ev.down = false;
            push_event(ev);
            return 1;
        case WM_RBUTTONDOWN:
            ev.kind = MouseQueueEvent::Button;
            ev.button = 1; ev.down = true;
            push_event(ev);
            return 1;
        case WM_RBUTTONUP:
            ev.kind = MouseQueueEvent::Button;
            ev.button = 1; ev.down = false;
            push_event(ev);
            return 1;
        case WM_MBUTTONDOWN:
            ev.kind = MouseQueueEvent::Button;
            ev.button = 2; ev.down = true;
            push_event(ev);
            return 1;
        case WM_MBUTTONUP:
            ev.kind = MouseQueueEvent::Button;
            ev.button = 2; ev.down = false;
            push_event(ev);
            return 1;
        case WM_MOUSEWHEEL: {
            ev.kind = MouseQueueEvent::Wheel;
            ev.wheel_delta = static_cast<int16_t>(HIWORD(m->mouseData));
            push_event(ev);
            return 1;
        }
        default:
            // Other mouse messages (XBUTTON, hover, etc.) pass through.
            break;
        }
    } else if (wp == WM_MOUSEWHEEL) {
        // Menu hidden — wheel can still drive freecam.
        const short delta = static_cast<short>(HIWORD(m->mouseData));
        if (mtr::freecam::active()) {
            mtr::freecam::accumulate_wheel(delta);
            return 1;
        }
    }

    return CallNextHookEx(nullptr, nCode, wp, lp);
}

DWORD WINAPI hook_thread(LPVOID) {
    // hMod can be NULL for LL hooks because they aren't injected into other
    // processes -- the callback runs in our own process on this thread.
    HHOOK h = SetWindowsHookExW(WH_MOUSE_LL, &mouse_ll_proc, nullptr, 0);
    if (!h) {
        mtr::log::info("input_hook: SetWindowsHookExW(WH_MOUSE_LL) failed err=%lu",
                       GetLastError());
        return 0;
    }
    g_hook.store(h);
    mtr::log::info("input_hook: WH_MOUSE_LL installed (hook=%p)", h);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnhookWindowsHookEx(h);
    g_hook.store(nullptr);
    return 0;
}

} // namespace

void install() {
    if (g_hook.load()) return;
    HANDLE t = CreateThread(nullptr, 0, &hook_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

// Drain accumulated wheel delta. Returns multiples of 120 (one notch =
// 120) — caller divides by 120 for ImGui's "wheel ticks" unit.
//
// Kept for back-compat with the existing menu.cpp NewFrame plumbing,
// but the menu now also calls drain_mouse_events() for the unified
// (clicks + moves + wheel) path. Both can run in the same frame; they
// pull from disjoint queues, so no risk of double-feeding ImGui.
int drain_wheel_delta() {
    return g_pending_wheel_delta.exchange(0);
}

// Drain the unified mouse-event queue (Pos + Button + Wheel events
// captured while UI was visible). Returns the number written to `out`,
// up to `max_out`. Caller dispatches each event to ImGui via
// io.AddMousePosEvent / io.AddMouseButtonEvent / io.AddMouseWheelEvent.
int drain_mouse_events(MouseQueueEvent* out, int max_out) {
    if (!out || max_out <= 0) return 0;
    std::scoped_lock lk(g_event_mu);
    const int n = static_cast<int>(
        g_event_queue.size() < static_cast<size_t>(max_out)
        ? g_event_queue.size()
        : static_cast<size_t>(max_out));
    for (int i = 0; i < n; ++i) out[i] = g_event_queue[i];
    g_event_queue.erase(g_event_queue.begin(),
                        g_event_queue.begin() + n);
    return n;
}

} // namespace mtr::input_hook
