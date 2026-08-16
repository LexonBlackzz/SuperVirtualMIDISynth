#include "PageAbout.h"
#include "ConfigDocument.h"
#include "Widgets.h"
#include "imgui.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace svms::cfg {

void DrawAboutPage(ConfigDocument& doc) {
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 20.0f);

    ImGui::PushFont(nullptr);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.88f, 0.92f, 1.0f));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0f);
    ImGui::Text("SuperVirtualMIDISynth V3");
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.56f, 0.59f, 0.62f, 1.0f));
    ImGui::Text("\"Semi-Professional*\" Audio Software "
                "tailored for Black MIDI");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("License: GPL-3.0");

#if defined(_WIN64)
    ImGui::Text("Architecture: x64");
#else
    ImGui::Text("Architecture: x86");
#endif

    ImGui::Text("Config schema version: 1");

    auto path = doc.GetActivePath();
    if (!path.empty()) {
        int len = WideCharToMultiByte(CP_UTF8, 0, path.data(),
                                      static_cast<int>(path.size()),
                                      nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            std::string pathStr(static_cast<size_t>(len), '\0');
            WideCharToMultiByte(CP_UTF8, 0, path.data(),
                                static_cast<int>(path.size()),
                                pathStr.data(), len, nullptr, nullptr);
            ImGui::TextDisabled("Config: %s", pathStr.c_str());
        }
    }

    ImGui::Spacing();
    ImGui::Text("Repository:");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.447f, 0.533f, 0.855f, 1.0f));
    ImGui::Text("github.com/LexonBlackzz/SuperVirtualMIDISynth");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.48f, 0.52f, 1.0f));
    ImGui::TextWrapped(
        "Built for MIDI files that make normal synthesizers "
        "reconsider their career choices.");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.37f, 0.40f, 0.44f, 1.0f));
    ImGui::Text("*Professionalism may decrease as NPS increases.");
    ImGui::PopStyleColor();
}

} // namespace svms::cfg
