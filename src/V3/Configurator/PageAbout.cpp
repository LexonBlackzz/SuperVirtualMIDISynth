#include "PageAbout.h"
#include "ConfigDocument.h"
#include "Widgets.h"
#include "imgui.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace svms::cfg {

void DrawAboutPage(ConfigDocument& doc, bool vsyncEnabled,
                   const FramePacingStats& pacing) {
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

    ImGui::Text("VSync: %s", vsyncEnabled ? "On (1 interval)" : "Off (uncapped)");
    if (pacing.frameCount > 0) {
        ImGui::Text("Frame pacing (%d frames): avg %.2f ms  "
                    "P95 %.2f ms  worst %.2f ms",
                    pacing.frameCount, static_cast<double>(pacing.avgMs),
                    static_cast<double>(pacing.p95Ms),
                    static_cast<double>(pacing.worstMs));

        ImGui::Spacing();

        float graphW = (std::min)(ImGui::GetContentRegionAvail().x, 360.0f);
        float graphH = 72.0f;
        ImVec2 graphPos = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(graphPos,
                          ImVec2(graphPos.x + graphW, graphPos.y + graphH),
                          ImGui::GetColorU32(ImVec4(0.06f, 0.07f, 0.085f, 1.0f)),
                          4.0f);
        dl->AddRect(graphPos,
                    ImVec2(graphPos.x + graphW, graphPos.y + graphH),
                    ImGui::GetColorU32(ImVec4(0.15f, 0.17f, 0.21f, 1.0f)),
                    4.0f, 0, 1.0f);

        // Frame-time histogram: 4 ms bins over 0..40 ms, with the 60/120
        // Hz frame-budget gridlines overlaid.
        constexpr float kBinMs = 4.0f;
        constexpr int kBins = 10;
        int maxCount = 1;
        for (int i = 0; i < kBins; ++i) {
            if (pacing.histogram[i] > maxCount) {
                maxCount = pacing.histogram[i];
            }
        }

        const float plotLeft = graphPos.x + 30.0f;
        const float plotRight = graphPos.x + graphW - 6.0f;
        const float plotTop = graphPos.y + 6.0f;
        const float plotBot = graphPos.y + graphH - 8.0f;
        const float binW = (plotRight - plotLeft) / kBins;

        for (int i = 0; i < kBins; ++i) {
            const float h = (plotBot - plotTop) *
                (static_cast<float>(pacing.histogram[i]) / maxCount);
            const float x0 = plotLeft + binW * i;
            dl->AddRectFilled(ImVec2(x0 + 2.0f, plotBot - h),
                              ImVec2(x0 + binW - 2.0f, plotBot),
                              ImGui::GetColorU32(
                                  ImVec4(0.447f, 0.533f, 0.855f, 0.75f)),
                              2.0f);
        }

        const float budgetsMs[3] = { 16.67f, 8.33f, 4.17f };
        for (int b = 0; b < 3; ++b) {
            if (budgetsMs[b] >= 40.0f) continue;
            const float x = plotLeft +
                (plotRight - plotLeft) * (budgetsMs[b] / 40.0f);
            dl->AddLine(ImVec2(x, plotTop), ImVec2(x, plotBot),
                        ImGui::GetColorU32(ImVec4(0.30f, 0.32f, 0.36f, 1.0f)),
                        1.0f);
        }

        char axis[32];
        snprintf(axis, sizeof(axis), "0");
        dl->AddText(ImVec2(plotLeft - 2.0f, plotBot),
                    ImGui::GetColorU32(ImVec4(0.42f, 0.45f, 0.50f, 1.0f)),
                    axis);
        snprintf(axis, sizeof(axis), "40ms");
        dl->AddText(ImVec2(plotRight - 26.0f, plotBot),
                    ImGui::GetColorU32(ImVec4(0.42f, 0.45f, 0.50f, 1.0f)),
                    axis);
        dl->AddText(ImVec2(graphPos.x + graphW * 0.5f - 64.0f, graphPos.y + 2.0f),
                    ImGui::GetColorU32(ImVec4(0.56f, 0.59f, 0.62f, 1.0f)),
                    "Frame-time histogram (4 ms bins)");

        ImGui::Dummy(ImVec2(graphW, graphH));
    } else {
        ImGui::TextDisabled("Frame pacing: collecting samples…");
    }

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
