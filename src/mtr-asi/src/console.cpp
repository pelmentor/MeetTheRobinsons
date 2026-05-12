#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include "imgui.h"

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::console {

namespace {

// Reverse-engineered VAs (rr01 in retail Wilbur.exe). Phase 1 RE 2026-05-05.
// Full reference: research/findings/symbol-table.md §Console / REPL.
constexpr uintptr_t kConsolePrintfVA   = 0x005873A0;  // print sink (routes through stolen-byte IAT thunk; destination decompilable)
constexpr uintptr_t kConsoleDispatchVA = 0x00588DB0;  // dispatch_line(state, line)
constexpr uintptr_t kConsoleStatePtrVA = 0x007415E0;  // void** at this addr

using PFN_ConsolePrintf = int  (__cdecl*)(void* state, const char* fmt, ...);
// __thiscall via __fastcall trick: 1st arg in ECX, 2nd arg (edx dummy) ignored,
// rest on stack. Same convention used elsewhere in mtr-asi (see d3d9_hook.cpp).
using PFN_DispatchLine  = char (__fastcall*)(void* this_, void* /*edx*/, const char* line);

PFN_ConsolePrintf g_orig_printf = nullptr;

// Producer = game thread inside console_printf hook. Consumer = render thread
// inside ImGui draw. In Wilbur both are the main thread, so contention should be
// near-zero -- but keep a mutex for correctness if the engine ever logs from a
// worker.
struct Ring {
    std::mutex mu;
    std::deque<std::string> lines;
    static constexpr size_t kMaxLines = 1024;

    void push(std::string s) {
        std::lock_guard<std::mutex> lk(mu);
        if (lines.size() >= kMaxLines) lines.pop_front();
        lines.push_back(std::move(s));
    }
};
Ring g_ring;

std::atomic<bool> g_visible{false};
std::atomic<bool> g_installed{false};

char g_input_buf[256] = {};
std::deque<std::string> g_history;
int  g_history_pos = -1;
bool g_scroll_to_bottom = true;
bool g_focus_input_next_frame = false;

// Hook: capture every console print into our ring buffer, then forward to the
// original sink so the game's existing log path (file / OutputDebugString)
// keeps working unchanged.
int __cdecl hk_console_printf(void* state, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    buf[n] = 0;

    int trim = n;
    while (trim > 0 && (buf[trim - 1] == '\n' || buf[trim - 1] == '\r')) {
        buf[--trim] = 0;
    }
    if (trim > 0) {
        g_ring.push(std::string(buf, trim));
        mtr::log::info("[console] %s", buf);
    }

    // Forward to original. "%s" + buf avoids re-formatting %s arguments that
    // were already consumed by our vsnprintf.
    if (g_orig_printf) {
        return g_orig_printf(state, "%s", buf);
    }
    return 0;
}

void* current_state() {
    return *reinterpret_cast<void**>(kConsoleStatePtrVA);
}

// Engine's tokenizer only splits on space (separator at 0x6C9198 is " ").
// Set-syntax is `<var> <value>`, no '='. But the LIST output formats vars as
// `[type] name = value`, which leads users to type `var = value` -- the engine
// then parses "= value" as the value, atof returns 0, and the cvar gets zeroed.
// Translate user-natural `var = value` into engine-native `var   value` here,
// but ONLY when the first word is not a built-in (so commands like
// `echo a=b` and `msg evt=x` keep their literal arguments).
std::string sanitize_for_dispatch(std::string line) {
    static const char* kBuiltins[] = {
        "exec", "global", "context", "end", "verbosity",
        "help", "contexts", "list", "save", "echo", "msg",
    };
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    size_t word_begin = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '=') ++i;
    std::string first(line.begin() + word_begin, line.begin() + i);

    for (const char* b : kBuiltins) {
        if (_stricmp(first.c_str(), b) == 0) return line;
    }
    for (char& c : line) if (c == '=') c = ' ';
    return line;
}

void dispatch_line_to_engine(const char* line) {
    void* state = current_state();
    if (!state) {
        g_ring.push("[mtr-asi] g_console_state is null -- engine not initialised yet");
        return;
    }
    std::string sanitized = sanitize_for_dispatch(line);
    auto fn = reinterpret_cast<PFN_DispatchLine>(kConsoleDispatchVA);
    fn(state, nullptr, sanitized.c_str());
}

int input_callback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        const int prev = g_history_pos;
        if (data->EventKey == ImGuiKey_UpArrow) {
            if (g_history_pos == -1) {
                g_history_pos = (int)g_history.size() - 1;
            } else if (g_history_pos > 0) {
                --g_history_pos;
            }
        } else if (data->EventKey == ImGuiKey_DownArrow) {
            if (g_history_pos != -1) {
                if (++g_history_pos >= (int)g_history.size()) {
                    g_history_pos = -1;
                }
            }
        }
        if (prev != g_history_pos) {
            const char* s = (g_history_pos == -1) ? ""
                                                  : g_history[g_history_pos].c_str();
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, s);
        }
    }
    return 0;
}

} // namespace

bool install() {
    if (g_installed.load()) return true;

    void* p = reinterpret_cast<void*>(kConsolePrintfVA);
    if (MH_CreateHook(p, &hk_console_printf,
                      reinterpret_cast<void**>(&g_orig_printf)) != MH_OK) {
        mtr::log::info("console: MH_CreateHook(console_printf @ %p) failed", p);
        return false;
    }
    if (MH_EnableHook(p) != MH_OK) {
        mtr::log::info("console: MH_EnableHook(console_printf) failed");
        return false;
    }
    g_installed = true;
    mtr::log::info("console: hook armed (sink=%p, dispatch=%p, state-ptr=%p)",
                   p, (void*)kConsoleDispatchVA, (void*)kConsoleStatePtrVA);

    g_ring.push("mtr-asi console ready. Try: help, contexts, list");
    g_ring.push("Set syntax: '<var> <value>'. '=' is also accepted (auto-translated).");
    return true;
}

bool is_visible()       { return g_visible.load(); }
void set_visible(bool v) { g_visible.store(v); if (v) g_focus_input_next_frame = true; }
bool toggle_visible()   {
    bool nv = !g_visible.load();
    set_visible(nv);
    return nv;
}

void poll_hotkey() {
    static bool prev = false;
    bool down = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    if (down && !prev) {
        bool v = toggle_visible();
        mtr::log::info("console: visible=%d (F2)", v ? 1 : 0);
    }
    prev = down;
}

void draw() {
    if (!g_visible.load()) return;

    ImGui::SetNextWindowSize(ImVec2(720, 360), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(40, 460), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("mtr-asi console (F2)", nullptr, ImGuiWindowFlags_NoCollapse)) {
        const float footer_h = ImGui::GetFrameHeightWithSpacing();
        if (ImGui::BeginChild("scroll", ImVec2(0, -footer_h), 0)) {
            std::lock_guard<std::mutex> lk(g_ring.mu);
            for (const auto& line : g_ring.lines) {
                ImGui::TextUnformatted(line.c_str());
            }
            if (g_scroll_to_bottom ||
                (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)) {
                ImGui::SetScrollHereY(1.0f);
            }
            g_scroll_to_bottom = false;
        }
        ImGui::EndChild();

        ImGui::Separator();
        if (g_focus_input_next_frame) {
            ImGui::SetKeyboardFocusHere();
            g_focus_input_next_frame = false;
        }
        ImGui::PushItemWidth(-1);
        const ImGuiInputTextFlags flags =
            ImGuiInputTextFlags_EnterReturnsTrue |
            ImGuiInputTextFlags_CallbackHistory |
            ImGuiInputTextFlags_EscapeClearsAll;
        if (ImGui::InputText("##cmd", g_input_buf, sizeof(g_input_buf),
                             flags, input_callback)) {
            std::string line(g_input_buf);
            if (!line.empty()) {
                g_ring.push("> " + line);
                mtr::log::info("[console] > %s", line.c_str());
                if (g_history.empty() || g_history.back() != line) {
                    g_history.push_back(line);
                    if (g_history.size() > 256) g_history.pop_front();
                }
                g_history_pos = -1;
                dispatch_line_to_engine(line.c_str());
                g_input_buf[0] = 0;
                g_scroll_to_bottom = true;
                g_focus_input_next_frame = true;
            }
        }
        ImGui::PopItemWidth();
    }
    ImGui::End();
}

} // namespace mtr::console
