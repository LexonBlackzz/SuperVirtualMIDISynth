#include "PageReverb.h"
#include "ConfigDocument.h"
#include "Widgets.h"
#include "imgui.h"
#include "Theme.h"
#include "../SVMSRuntimeLinkProtocol.h"
#include <cmath>

namespace svms::cfg {

void DrawReverbPage(ConfigDocument& doc) {
    PushEffectPageStyle();

    auto& w = doc.Working();
    static float animTime = 0.0f;
    animTime += ImGui::GetIO().DeltaTime;

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);

    {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 6));

        ImGui::PushStyleColor(ImGuiCol_FrameBg,
                              w.enableReverb
                                  ? ImVec4(0.15f, 0.35f, 0.55f, 0.5f)
                                  : ImVec4(0.15f, 0.16f, 0.19f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                              ImVec4(0.18f, 0.40f, 0.60f, 0.5f));

        bool reverbEnabled = w.enableReverb;
        if (ToggleSwitch("POWER", &reverbEnabled, "Enable the FDN reverb effect.")) {
            w.enableReverb = reverbEnabled;
            doc.MarkDirty();
            PushLiveBool(svms::RLCommandType::SetReverbEnabled, reverbEnabled);
        }
        auto& lc = GetLiveLinkContext();
        if (lc.connected) LiveBadge("Reverb toggle applied live via RuntimeLink");

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }

    ImGui::SameLine(ImGui::GetWindowWidth() * 0.3f);
    ImGui::PushFont(nullptr);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.78f, 0.95f, 1.0f));
    ImGui::Text("REVERB");
    ImGui::PopStyleColor();
    ImGui::PopFont();

    {
        auto& lc = GetLiveLinkContext();
        if (lc.connected) LiveBadge("All reverb params are applied live via RuntimeLink");
        AppliedStateBadge(lc.connected, lc.telemetry, w,
                          "Reverb group applied state vs working copy");
    }

    ImGui::SameLine(ImGui::GetWindowWidth() - 200.0f);
    {
        KnobState mixKnob = {
            w.reverbMix, 0.0f, 1.0f, 0.25f, "MIX", nullptr, 72.0f, 100.0f
        };
        if (RotaryKnob(mixKnob, "%.0f%%")) {
            w.reverbMix = mixKnob.value;
            doc.MarkDirty();
            PushLiveFloat(svms::RLCommandType::SetReverbMix, mixKnob.value);
        }
    }

    ImGui::Separator();
    ImGui::Spacing();

    float availW = ImGui::GetContentRegionAvail().x;
    float vizW = availW * 0.35f;
    float vizH = 240.0f;

    ImVec2 vizPos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 panelBg = ImVec4(0.065f, 0.075f, 0.090f, 1.0f);
    dl->AddRectFilled(vizPos,
                      ImVec2(vizPos.x + vizW, vizPos.y + vizH),
                      ImGui::GetColorU32(panelBg), 6.0f);
    dl->AddRect(vizPos,
                ImVec2(vizPos.x + vizW, vizPos.y + vizH),
                ImGui::GetColorU32(ImVec4(0.15f, 0.17f, 0.21f, 1.0f)),
                6.0f, 0, 1.0f);

    DrawReverbVisualizer(
        dl,
        ImVec2(vizPos.x + vizW * 0.5f, vizPos.y + vizH * 0.5f),
        vizW * 0.42f,
        w.reverbRoomSize, w.reverbDecay, w.reverbDiffusion,
        w.reverbWidth, w.reverbModDepth, animTime);

    ImGui::Dummy(ImVec2(vizW, vizH));
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12.0f);

    ImGui::BeginGroup();

    SectionHeader("SPACE");
    {
        float roomSize = w.reverbRoomSize;
        KnobState ks = { roomSize, 0.0f, 1.0f, 0.60f, "ROOM SIZE", nullptr, 58.0f, 100.0f };
        if (RotaryKnob(ks, "%.0f%%")) {
            w.reverbRoomSize = ks.value;
            doc.MarkDirty();
            PushLiveFloat(svms::RLCommandType::SetReverbRoomSize, ks.value);
        }
        ImGui::SameLine();
    }
    {
        float decay = w.reverbDecay;
        KnobState ks = { decay, 0.0f, 1.0f, 0.50f, "DECAY", nullptr, 58.0f, 100.0f };
        if (RotaryKnob(ks, "%.0f%%")) {
            w.reverbDecay = ks.value;
            doc.MarkDirty();
            PushLiveFloat(svms::RLCommandType::SetReverbDecay, ks.value);
        }
        ImGui::SameLine();
    }
    {
        float pd = w.reverbPreDelayMs;
        KnobState ks = { pd, 0.0f, 200.0f, 12.0f, "PRE-DELAY", "ms", 58.0f };
        if (RotaryKnob(ks, "%.1f")) {
            w.reverbPreDelayMs = ks.value;
            doc.MarkDirty();
            PushLiveFloat(svms::RLCommandType::SetReverbPreDelayMs, ks.value);
        }
    }

    ImGui::Spacing();
    SectionHeader("TEXTURE");
    {
        float diff = w.reverbDiffusion;
        KnobState ks = { diff, 0.0f, 1.0f, 0.70f, "DIFFUSION", nullptr, 52.0f, 100.0f };
        if (RotaryKnob(ks, "%.0f%%")) {
            w.reverbDiffusion = ks.value;
            doc.MarkDirty();
            PushLiveFloat(svms::RLCommandType::SetReverbDiffusion, ks.value);
        }
        ImGui::SameLine();
    }
    {
        float damp = w.reverbDamping;
        KnobState ks = { damp, 0.0f, 1.0f, 0.35f, "DAMPING", nullptr, 52.0f, 100.0f };
        if (RotaryKnob(ks, "%.0f%%")) {
            w.reverbDamping = ks.value;
            doc.MarkDirty();
            PushLiveFloat(svms::RLCommandType::SetReverbDamping, ks.value);
        }
    }

    ImGui::Spacing();
    SectionHeader("STEREO / BALANCE");
    {
        float width = w.reverbWidth;
        KnobState ks = { width, 0.0f, 1.0f, 1.0f, "WIDTH", nullptr, 52.0f, 100.0f };
        if (RotaryKnob(ks, "%.0f%%")) {
            w.reverbWidth = ks.value;
            doc.MarkDirty();
            PushLiveFloat(svms::RLCommandType::SetReverbWidth, ks.value);
        }
        ImGui::SameLine();
    }
    {
        float early = w.reverbEarlyLevel;
        KnobState ks = { early, 0.0f, 1.5f, 0.35f, "EARLY", nullptr, 52.0f };
        if (RotaryKnob(ks, "%.2f")) {
            w.reverbEarlyLevel = ks.value;
            doc.MarkDirty();
            PushLiveFloat(svms::RLCommandType::SetReverbEarlyLevel, ks.value);
        }
        ImGui::SameLine();
    }
    {
        float late = w.reverbLateLevel;
        KnobState ks = { late, 0.0f, 1.5f, 0.85f, "LATE", nullptr, 52.0f };
        if (RotaryKnob(ks, "%.2f")) {
            w.reverbLateLevel = ks.value;
            doc.MarkDirty();
            PushLiveFloat(svms::RLCommandType::SetReverbLateLevel, ks.value);
        }
    }

    ImGui::Spacing();
    SectionHeader("MODULATION");
    {
        float md = w.reverbModDepth;
        KnobState ks = { md, 0.0f, 1.0f, 0.30f, "DEPTH", nullptr, 52.0f, 100.0f };
        if (RotaryKnob(ks, "%.0f%%")) {
            w.reverbModDepth = ks.value;
            doc.MarkDirty();
            PushLiveFloat(svms::RLCommandType::SetReverbModDepth, ks.value);
        }
        ImGui::SameLine();
    }
    {
        float mr = w.reverbModRate;
        KnobState ks = { mr, 0.0f, 1.0f, 0.35f, "RATE", nullptr, 52.0f };
        if (RotaryKnob(ks, "%.2f")) {
            w.reverbModRate = ks.value;
            doc.MarkDirty();
            PushLiveFloat(svms::RLCommandType::SetReverbModRate, ks.value);
        }
    }

    ImGui::Spacing();
    SectionHeader("FILTER");
    {
        float lc = w.reverbLowCutHz;
        KnobState ks = { lc, 0.0f, 2000.0f, 70.0f, "LOW CUT", "Hz", 52.0f };
        if (RotaryKnob(ks, "%.0f")) {
            w.reverbLowCutHz = ks.value;
            doc.MarkDirty();
            PushLiveFloat(svms::RLCommandType::SetReverbLowCutHz, ks.value);
        }
        ImGui::SameLine();
    }
    {
        float hc = w.reverbHighCutHz;
        KnobState ks = { hc, 1000.0f, 20000.0f, 16000.0f, "HIGH CUT", "Hz", 52.0f };
        if (RotaryKnob(ks, "%.0f")) {
            w.reverbHighCutHz = ks.value;
            doc.MarkDirty();
            PushLiveFloat(svms::RLCommandType::SetReverbHighCutHz, ks.value);
        }
    }

    // RotaryKnob draws its text slightly below its interaction rectangle.
    // Reserve that tail explicitly so the final FILTER labels remain above
    // the fixed footer instead of being clipped at the bottom of the page.
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() + 12.0f));

    ImGui::EndGroup();

    PopEffectPageStyle();
}

} // namespace svms::cfg
