#include "PageLimiter.h"
#include "ConfigDocument.h"
#include "Widgets.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "Theme.h"
#include "../SVMSRuntimeLinkProtocol.h"
#include <cmath>

namespace svms::cfg {

static float LinearToDb(float linear) {
    return 20.0f * std::log10(std::max(linear, 0.001f));
}

void DrawLimiterPage(ConfigDocument& doc) {
    PushEffectPageStyle();

    auto& w = doc.Working();
    auto& lc = GetLiveLinkContext();
    const auto* t = lc.telemetry;

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);

    {
        bool limiterEnabled = w.limiterEnabled;
        if (ToggleSwitch("ENABLED", &limiterEnabled,
                         "Enable the transparent brick-wall limiter.")) {
            w.limiterEnabled = limiterEnabled;
            doc.MarkDirty();
            PushLiveBool(svms::RLCommandType::SetLimiterEnabled, limiterEnabled);
        }
        if (lc.connected) LiveBadge("Limiter toggle applied live via RuntimeLink");
    }

    ImGui::SameLine(ImGui::GetWindowWidth() * 0.3f);
    ImGui::PushFont(nullptr);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.75f, 0.55f, 1.0f));
    ImGui::Text("LIMITER");
    ImGui::PopStyleColor();
    ImGui::PopFont();

    if (lc.connected) LiveBadge("All limiter params are applied live via RuntimeLink");

    ImGui::Separator();
    ImGui::Spacing();

    float meterW = 40.0f;
    float meterH = 200.0f;
    float meterGap = 24.0f;

    auto drawMeterLabel = [&](const char* label, float x, float y) {
        ImVec2 ts = ImGui::CalcTextSize(label);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(x + meterW * 0.5f - ts.x * 0.5f, y + meterH + 4.0f),
            ImGui::GetColorU32(ImVec4(0.56f, 0.59f, 0.62f, 1.0f)), label);
    };

    ImVec2 meterStart = ImGui::GetCursorScreenPos();

    float livePeak = t ? t->renderPeak : 0.0f;
    float liveGR = t ? t->limiterGainReductionDb : 0.0f;

    DrawVerticalMeter("##input", livePeak, livePeak, ImVec2(meterW, meterH), true);
    drawMeterLabel("INPUT", meterStart.x, meterStart.y);

    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + meterGap);
    ImVec2 grPos = ImGui::GetCursorScreenPos();
    DrawGainReductionMeter("##gr", liveGR, ImVec2(meterW, meterH));
    drawMeterLabel("GAIN RED.", grPos.x, grPos.y);

    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + meterGap);
    ImVec2 outPos = ImGui::GetCursorScreenPos();
    DrawVerticalMeter("##output", livePeak, livePeak, ImVec2(meterW, meterH), true);
    drawMeterLabel("OUTPUT", outPos.x, outPos.y);

    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 24.0f);

    ImGui::BeginGroup();

    {
        float thresh = w.limiterThreshold;
        KnobState ks = { thresh, 0.1f, 1.0f, 0.95f, "THRESHOLD", nullptr, 72.0f, 1.0f, LinearToDb };
        if (RotaryKnob(ks, "%.1f dB")) {
            w.limiterThreshold = ks.value;
            doc.MarkDirty();
            PushLiveFloat(svms::RLCommandType::SetLimiterThreshold, ks.value);
        }
        ImGui::SameLine();
    }

    {
        float la = w.limiterLookaheadMs;
        KnobState ks = { la, 0.0f, 20.0f, 3.0f, "LOOKAHEAD", "ms", 58.0f };
        if (RotaryKnob(ks, "%.1f")) {
            w.limiterLookaheadMs = ks.value;
            doc.MarkDirty();
            PushLiveFloat(svms::RLCommandType::SetLimiterLookahead, ks.value);
        }
    }

    ImGui::Spacing();
    ImGui::Spacing();

    {
        float atk = w.limiterAttackMs;
        KnobState ks = { atk, 0.01f, 100.0f, 0.5f, "ATTACK", "ms", 58.0f };
        if (RotaryKnob(ks, "%.1f")) {
            w.limiterAttackMs = ks.value;
            doc.MarkDirty();
            PushLiveFloat(svms::RLCommandType::SetLimiterAttack, ks.value);
        }
        ImGui::SameLine();
    }
    {
        float rel = w.limiterReleaseMs;
        KnobState ks = { rel, 1.0f, 5000.0f, 100.0f, "RELEASE", "ms", 58.0f };
        if (RotaryKnob(ks, "%.1f")) {
            w.limiterReleaseMs = ks.value;
            doc.MarkDirty();
            PushLiveFloat(svms::RLCommandType::SetLimiterRelease, ks.value);
        }
    }

    ImGui::EndGroup();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (lc.connected && t) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.30f, 0.80f, 0.45f, 1.0f));
        ImGui::Text("Live limiter telemetry active");
        ImGui::PopStyleColor();

        ImGui::Text("Gain reduction: %.1f dB", liveGR);
        ImGui::Text("Render peak: %.4f (%.1f dB)", livePeak, LinearToDb(livePeak));
        ImGui::Text("Threshold: %.1f dB", LinearToDb(w.limiterThreshold));

        ImGui::Spacing();

        float graphW = ImGui::GetContentRegionAvail().x;
        float graphH = 80.0f;
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

        float grNorm = ImClamp(-liveGR / 24.0f, 0.0f, 1.0f);
        if (grNorm > 0.01f) {
            ImVec2 barBot(graphPos.x + 4.0f, graphPos.y + graphH - 4.0f);
            ImVec2 barTop(graphPos.x + 4.0f,
                          graphPos.y + graphH - 4.0f - (graphH - 8.0f) * grNorm);
            dl->AddRectFilled(barBot, barTop,
                              ImGui::GetColorU32(ImVec4(0.90f, 0.70f, 0.20f, 0.8f)),
                              2.0f);
        }

        ImVec2 textPos(graphPos.x + graphW * 0.5f - 60.0f,
                       graphPos.y + graphH * 0.5f - 8.0f);
        dl->AddText(textPos,
                    ImGui::GetColorU32(ImVec4(0.70f, 0.72f, 0.75f, 1.0f)),
                    "Gain reduction (dB)");

        ImGui::Dummy(ImVec2(graphW, graphH));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.56f, 0.59f, 0.62f, 1.0f));
        ImGui::Text("Live meter source: Runtime telemetry unavailable");
        ImGui::PopStyleColor();

        float graphW = ImGui::GetContentRegionAvail().x;
        float graphH = 80.0f;
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

        ImVec2 textPos(graphPos.x + graphW * 0.5f - 80.0f,
                       graphPos.y + graphH * 0.5f - 8.0f);
        dl->AddText(textPos,
                    ImGui::GetColorU32(ImVec4(0.40f, 0.43f, 0.48f, 1.0f)),
                    "Live gain-reduction telemetry unavailable");

        ImGui::Dummy(ImVec2(graphW, graphH));
    }

    PopEffectPageStyle();
}

} // namespace svms::cfg
