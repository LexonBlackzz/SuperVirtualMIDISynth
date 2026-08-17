#include "PageLimiter.h"
#include "ConfigDocument.h"
#include "Widgets.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "Theme.h"
#include "../SVMSRuntimeLinkProtocol.h"
#include <cmath>
#include <algorithm>

namespace svms::cfg {

static float LinearToDb(float linear) {
    return 20.0f * std::log10(std::max(linear, 0.001f));
}

static constexpr float kGrHistorySeconds = 10.0f;
static constexpr int kGrHistoryMaxPixels = 1024;

struct GrHistory {
    float buf[kGrHistoryMaxPixels] = {};
    int writePos = 0;
    int count = 0;
};

static void GrHistory_Push(GrHistory& h, float db) {
    h.buf[h.writePos] = db;
    h.writePos = (h.writePos + 1) % kGrHistoryMaxPixels;
    if (h.count < kGrHistoryMaxPixels) ++h.count;
}

void DrawLimiterPage(ConfigDocument& doc) {
    PushEffectPageStyle();

    auto& w = doc.Working();
    auto& lc = GetLiveLinkContext();
    const auto* t = lc.telemetry;

    static GrHistory grHist = {};

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);

    // ── Header row: ENABLED toggle + title + badges ────────────────────
    {
        bool limiterEnabled = w.limiterEnabled;
        if (ToggleSwitch("ENABLED", &limiterEnabled,
                         "Enable the transparent brick-wall limiter.")) {
            w.limiterEnabled = limiterEnabled;
            doc.MarkDirty();
            PushLiveBool(svms::RLCommandType::SetLimiterEnabled, limiterEnabled);
        }
    }
    ImGui::SameLine();
    if (lc.connected) LiveBadge("All limiter params applied live");
    AppliedStateBadge(lc.connected, lc.telemetry, w,
                      "Limiter group applied state vs working copy");

    ImGui::SameLine(ImGui::GetWindowWidth() * 0.3f);
    ImGui::PushFont(nullptr);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.75f, 0.55f, 1.0f));
    ImGui::Text("LIMITER");
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::Separator();
    ImGui::Spacing();

    // ── Meters + knobs using a table ───────────────────────────────────
    // Col 0: Input meter, Col 1: GR meter, Col 2: Output meter,
    // Col 3: knob group (threshold + timing controls)
    float meterH = 220.0f;
    float stereoBarW = 28.0f;
    float stereoGap = 8.0f;
    float stereoPairW = stereoBarW * 2.0f + stereoGap + 30.0f;
    float knobAreaW = 280.0f;
    float gap = 16.0f;
    float colW[4] = {
        stereoPairW + gap,
        60.0f + 30.0f + gap,
        stereoPairW + gap,
        knobAreaW
    };

    float liveInL = t ? t->limiterInputPeakL : 0.0f;
    float liveInR = t ? t->limiterInputPeakR : 0.0f;
    float liveOutL = t ? t->limiterOutputPeakL : 0.0f;
    float liveOutR = t ? t->limiterOutputPeakR : 0.0f;
    float liveGR = t ? t->limiterGainReductionDb : 0.0f;

    if (ImGui::BeginTable("##limiter_main", 4,
                          ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_NoBordersInBody |
                          ImGuiTableFlags_NoHostExtendX,
                          ImVec2(0, 0))) {
        ImGui::TableSetupColumn("input",  ImGuiTableColumnFlags_WidthFixed, colW[0]);
        ImGui::TableSetupColumn("gr",     ImGuiTableColumnFlags_WidthFixed, colW[1]);
        ImGui::TableSetupColumn("output", ImGuiTableColumnFlags_WidthFixed, colW[2]);
        ImGui::TableSetupColumn("knobs",  ImGuiTableColumnFlags_WidthFixed, colW[3]);
        ImGui::TableNextRow();

        // ── Column 0: Input meter (stereo L/R) ────────────────────────
        ImGui::TableNextColumn();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
        {
            ImVec2 mStart = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // L bar
            DrawVerticalMeter("##inL", liveInL, liveInL,
                              ImVec2(stereoBarW, meterH), false);
            // R bar
            ImGui::SameLine(0, stereoGap);
            DrawVerticalMeter("##inR", liveInR, liveInR,
                              ImVec2(stereoBarW, meterH), false);

            // dB scale (to the right of the pair)
            auto drawPairScale = [&](float x, float h) {
                auto tick = [&](float db, const char* text) {
                    float frac = (db + 48.0f) / 48.0f;
                    float y = mStart.y + h - h * frac;
                    dl->AddText(ImVec2(x, y - 5.0f),
                                ImGui::GetColorU32(
                                    ImVec4(0.45f, 0.48f, 0.52f, 0.8f)),
                                text);
                };
                tick(0.0f, "0");
                tick(-3.0f, "-3");
                tick(-6.0f, "-6");
                tick(-12.0f, "-12");
                tick(-24.0f, "-24");
                tick(-48.0f, "-48");
            };
            drawPairScale(mStart.x + stereoBarW * 2.0f + stereoGap + 4.0f,
                          meterH);

            // L/R labels
            float pairW = stereoBarW * 2.0f + stereoGap;
            ImVec2 lblL(mStart.x + stereoBarW * 0.5f - 4.0f,
                        mStart.y + meterH + 4.0f);
            dl->AddText(lblL,
                        ImGui::GetColorU32(ImVec4(0.56f, 0.59f, 0.62f, 1.0f)),
                        "L");
            ImVec2 lblR(mStart.x + stereoBarW + stereoGap + stereoBarW * 0.5f - 4.0f,
                        mStart.y + meterH + 4.0f);
            dl->AddText(lblR,
                        ImGui::GetColorU32(ImVec4(0.56f, 0.59f, 0.62f, 1.0f)),
                        "R");
            ImVec2 lblMain(mStart.x + pairW * 0.5f - 16.0f,
                           mStart.y + meterH + 16.0f);
            dl->AddText(lblMain,
                        ImGui::GetColorU32(ImVec4(0.56f, 0.59f, 0.62f, 1.0f)),
                        "INPUT");

            ImGui::Dummy(ImVec2(stereoPairW, meterH + 32.0f));
        }

        // ── Column 1: Gain Reduction meter ─────────────────────────────
        ImGui::TableNextColumn();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
        {
            ImVec2 mStart = ImGui::GetCursorScreenPos();
            DrawGainReductionMeter("##gr", liveGR, ImVec2(60.0f, meterH));

            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 lblPos(mStart.x + 60.0f * 0.5f - 30.0f,
                          mStart.y + meterH + 16.0f);
            dl->AddText(lblPos,
                        ImGui::GetColorU32(ImVec4(0.56f, 0.59f, 0.62f, 1.0f)),
                        "GAIN RED.");
        }

        // ── Column 2: Output meter (stereo L/R) ───────────────────────
        ImGui::TableNextColumn();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
        {
            ImVec2 mStart = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // L bar
            DrawVerticalMeter("##outL", liveOutL, liveOutL,
                              ImVec2(stereoBarW, meterH), false);
            // R bar
            ImGui::SameLine(0, stereoGap);
            DrawVerticalMeter("##outR", liveOutR, liveOutR,
                              ImVec2(stereoBarW, meterH), false);

            // dB scale
            auto drawPairScale = [&](float x, float h) {
                auto tick = [&](float db, const char* text) {
                    float frac = (db + 48.0f) / 48.0f;
                    float y = mStart.y + h - h * frac;
                    dl->AddText(ImVec2(x, y - 5.0f),
                                ImGui::GetColorU32(
                                    ImVec4(0.45f, 0.48f, 0.52f, 0.8f)),
                                text);
                };
                tick(0.0f, "0");
                tick(-3.0f, "-3");
                tick(-6.0f, "-6");
                tick(-12.0f, "-12");
                tick(-24.0f, "-24");
                tick(-48.0f, "-48");
            };
            drawPairScale(mStart.x + stereoBarW * 2.0f + stereoGap + 4.0f,
                          meterH);

            // L/R labels
            float pairW = stereoBarW * 2.0f + stereoGap;
            ImVec2 lblL(mStart.x + stereoBarW * 0.5f - 4.0f,
                        mStart.y + meterH + 4.0f);
            dl->AddText(lblL,
                        ImGui::GetColorU32(ImVec4(0.56f, 0.59f, 0.62f, 1.0f)),
                        "L");
            ImVec2 lblR(mStart.x + stereoBarW + stereoGap + stereoBarW * 0.5f - 4.0f,
                        mStart.y + meterH + 4.0f);
            dl->AddText(lblR,
                        ImGui::GetColorU32(ImVec4(0.56f, 0.59f, 0.62f, 1.0f)),
                        "R");
            ImVec2 lblMain(mStart.x + pairW * 0.5f - 20.0f,
                           mStart.y + meterH + 16.0f);
            dl->AddText(lblMain,
                        ImGui::GetColorU32(ImVec4(0.56f, 0.59f, 0.62f, 1.0f)),
                        "OUTPUT");

            ImGui::Dummy(ImVec2(stereoPairW, meterH + 32.0f));
        }

        // ── Column 3: Four knobs in a single row ───────────────────────
        ImGui::TableNextColumn();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
        {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.70f, 0.72f, 0.75f, 1.0f));
            ImGui::Text("CONTROLS");
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();

        {
            float thresh = w.limiterThreshold;
            KnobState ks = { thresh, 0.1f, 1.0f, 0.95f, "THRESHOLD", nullptr,
                             72.0f, 1.0f, LinearToDb };
            if (RotaryKnob(ks, "%.1f dB")) {
                w.limiterThreshold = ks.value;
                doc.MarkDirty();
                PushLiveFloat(svms::RLCommandType::SetLimiterThreshold, ks.value);
            }
        }

        ImGui::SameLine(0, 8.0f);
        {
            float la = w.limiterLookaheadMs;
            KnobState ks = { la, 0.0f, 20.0f, 3.0f, "LOOKAHEAD", "ms", 60.0f };
            if (RotaryKnob(ks, "%.1f")) {
                w.limiterLookaheadMs = ks.value;
                doc.MarkDirty();
                PushLiveFloat(svms::RLCommandType::SetLimiterLookahead, ks.value);
            }
        }

        ImGui::SameLine(0, 8.0f);
        {
            float atk = w.limiterAttackMs;
            KnobState ks = { atk, 0.01f, 100.0f, 0.5f, "ATTACK", "ms", 60.0f };
            if (RotaryKnob(ks, "%.1f")) {
                w.limiterAttackMs = ks.value;
                doc.MarkDirty();
                PushLiveFloat(svms::RLCommandType::SetLimiterAttack, ks.value);
            }
        }

        ImGui::SameLine(0, 8.0f);
        {
            float rel = w.limiterReleaseMs;
            KnobState ks = { rel, 1.0f, 5000.0f, 100.0f, "RELEASE", "ms", 60.0f };
            if (RotaryKnob(ks, "%.1f")) {
                w.limiterReleaseMs = ks.value;
                doc.MarkDirty();
                PushLiveFloat(svms::RLCommandType::SetLimiterRelease, ks.value);
            }
        }

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Live telemetry readout ─────────────────────────────────────────
    if (lc.connected && t) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.30f, 0.80f, 0.45f, 1.0f));
        ImGui::Text("Live limiter telemetry active");
        ImGui::PopStyleColor();

        ImGui::Text("Input peak:  L %.3f (%.1f dB)  R %.3f (%.1f dB)",
                    liveInL, LinearToDb(liveInL), liveInR, LinearToDb(liveInR));
        ImGui::Text("Output peak: L %.3f (%.1f dB)  R %.3f (%.1f dB)",
                    liveOutL, LinearToDb(liveOutL), liveOutR, LinearToDb(liveOutR));
        ImGui::Text("Gain reduction: %.1f dB", liveGR);
        ImGui::Text("Threshold: %.1f dB", LinearToDb(w.limiterThreshold));

        // Push GR into the scrolling history
        GrHistory_Push(grHist, liveGR);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.56f, 0.59f, 0.62f, 1.0f));
        ImGui::Text("Live meter source: Runtime telemetry unavailable");
        ImGui::PopStyleColor();

        GrHistory_Push(grHist, 0.0f);
    }

    ImGui::Spacing();

    // ── Scrolling 10-second gain-reduction history ─────────────────────
    {
        float graphW = ImGui::GetContentRegionAvail().x;
        float graphH = 120.0f;
        ImVec2 graphPos = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Background
        dl->AddRectFilled(graphPos,
                          ImVec2(graphPos.x + graphW, graphPos.y + graphH),
                          ImGui::GetColorU32(ImVec4(0.06f, 0.07f, 0.085f, 1.0f)),
                          4.0f);
        dl->AddRect(graphPos,
                    ImVec2(graphPos.x + graphW, graphPos.y + graphH),
                    ImGui::GetColorU32(ImVec4(0.15f, 0.17f, 0.21f, 1.0f)),
                    4.0f, 0, 1.0f);

        const float plotLeft = graphPos.x + 34.0f;
        const float plotRight = graphPos.x + graphW - 8.0f;
        const float plotTop = graphPos.y + 8.0f;
        const float plotBot = graphPos.y + graphH - 18.0f;

        // Graticule: 6 dB gridlines with labels
        for (int db = 6; db <= 24; db += 6) {
            float y = plotBot - (plotBot - plotTop) * (static_cast<float>(db) / 24.0f);
            dl->AddLine(ImVec2(plotLeft, y), ImVec2(plotRight, y),
                        ImGui::GetColorU32(ImVec4(0.18f, 0.20f, 0.24f, 1.0f)),
                        1.0f);
            char lbl[16];
            snprintf(lbl, sizeof(lbl), "-%d dB", db);
            dl->AddText(ImVec2(graphPos.x + 4.0f, y - 7.0f),
                        ImGui::GetColorU32(ImVec4(0.42f, 0.45f, 0.50f, 1.0f)),
                        lbl);
        }

        // Draw the GR history as a filled polygon
        if (grHist.count >= 2) {
            const int n = grHist.count;
            const float plotW = plotRight - plotLeft;
            const float plotH = plotBot - plotTop;

            // Build path points
            ImVec2 pts[kGrHistoryMaxPixels + 2];
            int numPts = 0;

            for (int i = 0; i < n; ++i) {
                const int idx = (grHist.writePos - n + i + kGrHistoryMaxPixels)
                                % kGrHistoryMaxPixels;
                const float gr = grHist.buf[idx];
                const float x = plotLeft + plotW * (static_cast<float>(i) / (n - 1));
                const float grNorm = ImClamp(-gr / 24.0f, 0.0f, 1.0f);
                const float y = plotBot - plotH * grNorm;
                pts[numPts++] = ImVec2(x, y);
            }

            // Close the polygon along the bottom
            pts[numPts++] = ImVec2(plotRight, plotBot);
            pts[numPts++] = ImVec2(plotLeft, plotBot);

            // Filled area
            dl->AddConvexPolyFilled(pts, numPts,
                ImGui::GetColorU32(ImVec4(0.90f, 0.70f, 0.20f, 0.15f)));

            // Line on top
            for (int i = 0; i < numPts - 3; ++i) {
                dl->AddLine(pts[i], pts[i + 1],
                            ImGui::GetColorU32(ImVec4(0.90f, 0.70f, 0.20f, 0.9f)),
                            1.5f);
            }
        }

        // Axis labels
        dl->AddText(ImVec2(plotRight - 40.0f, plotBot + 4.0f),
                    ImGui::GetColorU32(ImVec4(0.42f, 0.45f, 0.50f, 1.0f)),
                    "10 s ago");
        dl->AddText(ImVec2(plotLeft, plotBot + 4.0f),
                    ImGui::GetColorU32(ImVec4(0.42f, 0.45f, 0.50f, 1.0f)),
                    "now");

        dl->AddText(ImVec2(plotLeft + (plotRight - plotLeft) * 0.5f - 56.0f,
                           graphPos.y + 2.0f),
                    ImGui::GetColorU32(ImVec4(0.56f, 0.59f, 0.62f, 1.0f)),
                    "Gain reduction (dB)");

        ImGui::Dummy(ImVec2(graphW, graphH));
    }

    ImGui::Spacing();

    // ── Callback budget stats ──────────────────────────────────────────
    if (lc.connected && t) {
        if (t->overBudgetCallbacks > 0u || t->maxConsecutiveOverBudget > 0u) {
            ImGui::TextDisabled(
                "Callback budget: P95 %.0f%%  P99 %.0f%%  P999 %.0f%%  "
                "over-budget %llu (max streak %u)",
                static_cast<double>(t->callbackP95Percent),
                static_cast<double>(t->callbackP99Percent),
                static_cast<double>(t->callbackP999Percent),
                static_cast<unsigned long long>(t->overBudgetCallbacks),
                t->maxConsecutiveOverBudget);
        } else {
            ImGui::TextDisabled(
                "Callback budget: P95 %.0f%%  P99 %.0f%%  P999 %.0f%%",
                static_cast<double>(t->callbackP95Percent),
                static_cast<double>(t->callbackP99Percent),
                static_cast<double>(t->callbackP999Percent));
        }
    }

    PopEffectPageStyle();
}

} // namespace svms::cfg
