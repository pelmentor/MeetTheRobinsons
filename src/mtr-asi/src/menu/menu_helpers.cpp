// Section + button helpers shared by every tab.
//
// Style convention (read this before adding new UI):
//
//   RULE: never paste paragraph-length technical text directly into the
//   section body. Walls of TextWrapped clutter the menu and bury the
//   controls users care about. ALWAYS use one of these helpers instead:
//
//     info_pill(text)
//         emits "(?)" — hover shows `text`. Use next to a control or
//         after SameLine() to attach background context.
//
//     heading_with_info(title, details)
//         one-line section sub-heading + (?) pill. Call AT MOST ONCE per
//         section, immediately after section_begin(...), to give the
//         section a brief framing line with the technical detail tucked
//         away in the tooltip.
//
//     primary_button / warning_button / danger_button
//         colour-coded buttons. Reserve for the *primary* user-facing
//         action of a section; default ImGui blue stays for everything
//         else. Don't use these for engineering-diagnostic buttons.

#include "menu_internal.h"
#include "imgui.h"

namespace mtr::menu::detail {

bool section_begin(const char* title) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.18f, 0.32f, 0.50f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.22f, 0.38f, 0.58f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.26f, 0.45f, 0.66f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 6.0f));
    const bool open = ImGui::CollapsingHeader(title);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    if (open) {
        ImGui::Indent(8.0f);
        ImGui::Spacing();
    }
    return open;
}

void section_end(bool was_open) {
    if (was_open) {
        ImGui::Spacing();
        ImGui::Unindent(8.0f);
    }
}

void info_pill(const char* tooltip_text) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(tooltip_text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void heading_with_info(const char* title, const char* details) {
    ImGui::TextUnformatted(title);
    ImGui::SameLine();
    info_pill(details);
}

bool primary_button(const char* label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.55f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.65f, 0.26f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.26f, 0.75f, 0.30f, 1.0f));
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return r;
}

bool warning_button(const char* label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.45f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.55f, 0.14f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.75f, 0.65f, 0.18f, 1.0f));
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return r;
}

bool danger_button(const char* label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.22f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.75f, 0.26f, 0.26f, 1.0f));
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return r;
}

} // namespace mtr::menu::detail
