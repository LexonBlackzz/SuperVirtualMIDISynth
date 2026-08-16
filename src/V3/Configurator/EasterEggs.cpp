#include "EasterEggs.h"
#include "imgui.h"
#include <random>
#include <string>
#include <algorithm>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace svms::cfg {

EasterEggState RollEasterEggs(int argc, char** argv) {
    EasterEggState state;

    bool forceMegaFucker = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--force-megafucker") {
            forceMegaFucker = true;
            break;
        }
    }
    if (!forceMegaFucker) {
        wchar_t envVal[8]{};
        if (GetEnvironmentVariableW(L"SVMS_FORCE_MEGAFUCKER", envVal, 8) > 0) {
            std::wstring lower(envVal);
            std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
            if (lower == L"1" || lower == L"true" || lower == L"yes")
                forceMegaFucker = true;
        }
    }

    if (forceMegaFucker) {
        state.megaFuckerDac = true;
        state.realDeviceName = "Default Windows Output Device";
        return state;
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(1, 25000);
    if (dist(gen) == 1) {
        state.megaFuckerDac = true;
        state.realDeviceName = "Default Windows Output Device";
    }

    return state;
}

void ShowMegaFuckerNotification(const EasterEggState& state, bool* open) {
    if (!state.megaFuckerDac || !open || !*open) return;

    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Always);
    if (ImGui::Begin("EASTER EGG FOUND", open,
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.20f, 1.0f));
        ImGui::Text("MegaFucker DAC Pro 9000");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::TextWrapped("Your actual audio device has NOT changed.");
        ImGui::TextWrapped("This is display-only. Nothing is broken.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextDisabled("Actual device: %s", state.realDeviceName.c_str());
        ImGui::Spacing();
    }
    ImGui::End();
}

} // namespace svms::cfg
