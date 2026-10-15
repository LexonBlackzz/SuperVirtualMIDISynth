#include "PageAbout.h"
#include "ConfigDocument.h"
#include "Widgets.h"
#include "UpdateService.h"
#include "SVMSBuildInfo.h"
#include "imgui.h"

#include "PlatformUtil.h"

namespace svms::cfg {

void DrawAboutPage(ConfigDocument& doc, UpdateService& updates) {
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 20.0f);

    ImGui::PushFont(nullptr);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.88f, 0.92f, 1.0f));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0f);
    ImGui::Text("SuperVirtualMIDISynth V3");
    ImGui::PopStyleColor();
    ImGui::PopFont();

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
    ImGui::Text("Version: %s (build %u, %s)",
                svms::build::kProductVersion,
                svms::build::kBuildNumber,
                svms::build::kReleaseChannel);
    ImGui::TextDisabled("Source: %s", svms::build::kGitCommit);

    ImGui::Spacing();
    SectionHeader("UPDATES");
    const UpdateSnapshot update = updates.GetSnapshot();
    if (update.status == UpdateStatus::Checking) {
        ImGui::TextDisabled("Checking GitHub releases...");
    } else {
        const char* label = update.status == UpdateStatus::Idle
            ? "Check for updates" : "Check again";
        if (ImGui::Button(label))
            updates.CheckAsync(update.status != UpdateStatus::Idle);
    }
    if (update.status == UpdateStatus::Available) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImVec4(0.35f, 0.85f, 0.50f, 1.0f));
        ImGui::Text("%u.%u.%u is available%s", update.major, update.minor,
                    update.patch, update.fromCache ? " (cached)" : "");
        ImGui::PopStyleColor();
        if (!update.releaseUrl.empty() && ImGui::Button("Open release page")) {
            util::OpenExternal(update.releaseUrl);
        }
        ImGui::SameLine();
        ImGui::TextDisabled(
            "Automatic install requires a configured signed manifest.");
    } else if (update.status == UpdateStatus::UpToDate) {
        ImGui::SameLine();
        ImGui::TextDisabled("Up to date%s",
                            update.fromCache ? " (cached)" : "");
    } else if (update.status == UpdateStatus::Failed) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImVec4(0.92f, 0.48f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", update.message.c_str());
        ImGui::PopStyleColor();
    }

    auto path = doc.GetActivePath();
    if (!path.empty()) {
        const std::string pathStr = util::WideToUtf8(path);
        if (!pathStr.empty())
            ImGui::TextDisabled("Config: %s", pathStr.c_str());
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

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.56f, 0.59f, 0.62f, 1.0f));
    ImGui::TextWrapped("Semi-Professional software... for Black MIDI");
    ImGui::PopStyleColor();

    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.37f, 0.40f, 0.44f, 1.0f));
    ImGui::Text("*Professionalism may decrease as NPS increases.");
    ImGui::PopStyleColor();
}

} // namespace svms::cfg
