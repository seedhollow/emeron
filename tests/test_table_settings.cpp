#include "TestHarness.h"

#include <cmath>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>

// A stretch column whose saved weight is NaN collapses to nothing, and every
// wrapped line in it then breaks after each character -- the Log panel did
// this. ImGui stored the NaN itself: two of its weight formulas divide by a
// width that can be 0 (a panel laid out while hidden or squeezed narrower than
// its fixed columns), and it read the NaN back from imgui.ini on every launch.
// vendor/imgui carries a small patch for both; these tests pin it down.

namespace {

struct HeadlessImGui {
    ImGuiContext* context = nullptr;
    HeadlessImGui() {
        context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2{1400.0F, 900.0F};
        io.DeltaTime = 1.0F / 60.0F;
        io.Fonts->AddFontDefault();
        unsigned char* pixels = nullptr;
        int w = 0;
        int h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context); }
    HeadlessImGui(const HeadlessImGui&) = delete;
    HeadlessImGui& operator=(const HeadlessImGui&) = delete;
};

// The Log panel's table, as LogPanel.cpp builds it. Returns the width the
// Message column was given this frame.
float drawLogTable(float windowWidth) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2{0.0F, 0.0F});
    ImGui::SetNextWindowSize(ImVec2{windowWidth, 400.0F});
    float messageWidth = -1.0F;
    if (ImGui::Begin("Log")) {
        constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                           ImGuiTableFlags_Resizable |
                                           ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##log", 4, kFlags)) {
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 90.0F);
            ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 74.0F);
            ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 90.0F);
            ImGui::TableSetupColumn("Message");
            ImGui::TableHeadersRow();
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("08:33:56.217");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("INFO");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("logcat");
            ImGui::TableNextColumn();
            ImGui::TextWrapped("streaming log");
            messageWidth = ImGui::GetCurrentTable()->Columns[3].WidthGiven;
            ImGui::EndTable();
        }
    }
    ImGui::End();
    ImGui::Render();
    return messageWidth;
}

}  // namespace

TEST_CASE("a NaN column weight saved in imgui.ini no longer collapses the column") {
    HeadlessImGui imgui;
    // Verbatim from a user's ~/.emeron/imgui.ini.
    const char* ini =
        "[Table][0x584826A8,4]\n"
        "RefScale=13\n"
        "Column 0  Width=90 ID=0xDE21DBF3\n"
        "Column 1  Width=74 ID=0xFBE609D2\n"
        "Column 2  Width=90 ID=0x26563918\n"
        "Column 3  Weight=nan ID=0xA5F9976D\n";
    ImGui::LoadIniSettingsFromMemory(ini);

    float width = 0.0F;
    for (int frame = 0; frame < 3; ++frame) width = drawLogTable(1000.0F);
    CHECK(std::isfinite(width));
    CHECK(width > 400.0F);  // the rest of a 1000 px window after ~254 px of fixed columns
}

TEST_CASE("laying a table out at zero width does not poison its stretch weight") {
    HeadlessImGui imgui;
    // First frames with no room at all, as for a panel that starts out as a
    // hidden dock tab or squeezed narrower than its fixed columns...
    for (int frame = 0; frame < 3; ++frame) drawLogTable(1.0F);
    // ...then a normal width.
    float width = 0.0F;
    for (int frame = 0; frame < 3; ++frame) width = drawLogTable(1000.0F);
    CHECK(std::isfinite(width));
    CHECK(width > 400.0F);

    const float weight = ImGui::GetCurrentContext()->Tables.GetByIndex(0)->Columns[3].StretchWeight;
    CHECK(std::isfinite(weight));
    CHECK(weight > 0.0F);
}
