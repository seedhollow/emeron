#pragma once

// Small layout helpers shared by the panels.

#include <imgui.h>

#include "app/Theme.h"

namespace em::widgets {

// SameLine(), or a new line when an item of `width` would not fit -- keeps a
// toolbar usable in a narrow dock instead of clipping its right end.
inline void sameLineOrWrap(float width, float spacing = -1.0F) {
    ImGui::SameLine(0.0F, spacing);
    if (ImGui::GetContentRegionAvail().x < width) ImGui::NewLine();
}

[[nodiscard]] inline float iconButtonWidth(const char* label) {
    return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0F;
}

// An icon button that stays highlighted while `value` is on. Returns true when
// it was toggled this frame.
inline bool toggleButton(const char* icon, bool* value, const char* tooltip) {
    const bool on = *value;
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::palette().accent);
    }
    const bool pressed = ImGui::Button(icon);
    if (on) ImGui::PopStyleColor(2);
    if (pressed) *value = !*value;
    ImGui::SetItemTooltip("%s: %s", tooltip, *value ? "on" : "off");
    return pressed;
}

}  // namespace em::widgets

namespace em::widgets {

// Tooltip for the item just drawn, in one consistent shape: the technical term
// the plain label stands for (highlighted), then what it means. Labels in the
// UI are written for someone who has not memorised Android's vocabulary; the
// tooltip is where the precise name lives, so nothing is lost for those who
// have.
inline void termTooltip(const char* term, const char* explanation) {
    if (!ImGui::BeginItemTooltip()) return;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0F);
    ImGui::TextColored(theme::palette().accent, "%s", term);
    if (explanation != nullptr && explanation[0] != '\0') {
        ImGui::Spacing();
        ImGui::TextUnformatted(explanation);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// A muted plain-language label carrying a termTooltip.
inline void termLabel(const char* plain, const char* term, const char* explanation) {
    ImGui::TextDisabled("%s", plain);
    termTooltip(term, explanation);
}

// TableHeadersRow() with a termTooltip per column: `terms` and `explanations`
// are indexed by column; a null term means no tooltip for that column.
inline void headersRowWithTooltips(const char* const* terms, const char* const* explanations,
                                   int columns) {
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (int c = 0; c < columns; ++c) {
        if (!ImGui::TableSetColumnIndex(c)) continue;
        ImGui::TableHeader(ImGui::TableGetColumnName(c));
        if (terms[c] != nullptr) termTooltip(terms[c], explanations[c]);
    }
}

}  // namespace em::widgets
