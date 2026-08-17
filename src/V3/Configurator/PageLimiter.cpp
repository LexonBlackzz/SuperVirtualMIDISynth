#include "PageLimiter.h"
#include "ConfigDocument.h"
#include "Widgets.h"
#include "imgui.h"
#include "Theme.h"
#include "../SVMSRuntimeLinkProtocol.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace svms::cfg {
namespace {

constexpr float kMeterFloorDb = -60.0f;
constexpr float kGrMaxDb = 24.0f;
constexpr int kGrHistoryCapacity = 320; // ~10.7 s at 30 Hz

float LinearToDb(float linear) {
    if (!std::isfinite(linear) || linear <= 0.000001f) return kMeterFloorDb;
    return (std::max)(kMeterFloorDb, 20.0f * std::log10(linear));
}

float MeterNorm(float linear) {
    const float db = LinearToDb(linear);
    return ImClamp((db - kMeterFloorDb) / -kMeterFloorDb, 0.0f, 1.0f);
}

struct GrHistory {
    float values[kGrHistoryCapacity]{};
    int writePos = 0;
    int count = 0;
    float accumulator = 0.0f;

    void Push(float value) {
        values[writePos] = ImClamp(value, 0.0f, kGrMaxDb);
        writePos = (writePos + 1) % kGrHistoryCapacity;
        if (count < kGrHistoryCapacity) ++count;
    }

    float AtOldestOffset(int i) const {
        const int start = (writePos - count + kGrHistoryCapacity) % kGrHistoryCapacity;
        return values[(start + i) % kGrHistoryCapacity];
    }
};

void DrawLevelBar(const char* id, float linear, const ImVec2& size) {
    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const ImU32 bg = ImGui::GetColorU32(ImVec4(0.055f, 0.062f, 0.074f, 1.0f));
    const ImU32 border = ImGui::GetColorU32(ImVec4(0.16f, 0.18f, 0.22f, 1.0f));
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg, 3.0f);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), border, 3.0f);

    const float norm = MeterNorm(linear);
    if (norm > 0.0f) {
        const float top = pos.y + size.y * (1.0f - norm);
        const float db = LinearToDb(linear);
        const ImVec4 c = db < -12.0f
            ? ImVec4(0.25f, 0.76f, 0.43f, 0.95f)
            : (db < -3.0f
                ? ImVec4(0.92f, 0.72f, 0.18f, 0.95f)
                : ImVec4(0.90f, 0.30f, 0.28f, 0.95f));
        dl->AddRectFilled(ImVec2(pos.x + 2.0f, top),
                          ImVec2(pos.x + size.x - 2.0f, pos.y + size.y - 2.0f),
                          ImGui::GetColorU32(c), 2.0f);
    }

    ImGui::Dummy(size);
    ImGui::PopID();
}

void DrawGrBar(const char* id, float reductionDb, const ImVec2& size) {
    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const ImU32 bg = ImGui::GetColorU32(ImVec4(0.055f, 0.062f, 0.074f, 1.0f));
    const ImU32 border = ImGui::GetColorU32(ImVec4(0.16f, 0.18f, 0.22f, 1.0f));
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg, 3.0f);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), border, 3.0f);

    const float norm = ImClamp(reductionDb / kGrMaxDb, 0.0f, 1.0f);
    if (norm > 0.0f) {
        dl->AddRectFilled(ImVec2(pos.x + 2.0f, pos.y + 2.0f),
                          ImVec2(pos.x + size.x - 2.0f,
                                 pos.y + 2.0f + (size.y - 4.0f) * norm),
                          ImGui::GetColorU32(ImVec4(0.94f, 0.68f, 0.16f, 0.95f)),
                          2.0f);
    }

    ImGui::Dummy(size);
    ImGui::PopID();
}

void DrawDbScale(float x, float top, float height) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float ticks[] = { 0.0f, -3.0f, -6.0f, -12.0f, -24.0f, -48.0f, -60.0f };
    for (float db : ticks) {
        const float norm = (db - kMeterFloorDb) / -kMeterFloorDb;
        const float y = top + height * (1.0f - norm);
        char text[16];
        std::snprintf(text, sizeof(text), "%.0f", db);
        dl->AddText(ImVec2(x, y - ImGui::GetFontSize() * 0.5f),
                    ImGui::GetColorU32(ImVec4(0.43f, 0.46f, 0.51f, 0.9f)), text);
    }
}

void DrawMeterBank(float inL, float inR, float gr, float outL, float outR,
                   float height) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float labelColumn = 26.0f;
    const float groupGap = 14.0f;
    const float usable = (std::max)(220.0f, avail - labelColumn * 2.0f - groupGap * 2.0f);
    const float groupW = usable / 3.0f;
    const float stereoGap = 5.0f;
    const float stereoBarW = (std::max)(18.0f, (groupW - 24.0f - stereoGap) * 0.5f);
    const float grBarW = (std::max)(30.0f, groupW * 0.46f);

    if (!ImGui::BeginTable("##meter_bank", 3, ImGuiTableFlags_SizingStretchSame)) return;

    auto stereoGroup = [&](const char* name, const char* idPrefix, float l, float r) {
        ImGui::TableNextColumn();
        const float colStart = ImGui::GetCursorPosX();
        const float colAvail = ImGui::GetContentRegionAvail().x;
        const float barsWidth = stereoBarW * 2.0f + stereoGap;
        ImGui::SetCursorPosX(colStart + (colAvail - (barsWidth + labelColumn)) * 0.5f);
        const ImVec2 screen = ImGui::GetCursorScreenPos();

        char idL[32], idR[32];
        std::snprintf(idL, sizeof(idL), "%sL", idPrefix);
        std::snprintf(idR, sizeof(idR), "%sR", idPrefix);
        DrawLevelBar(idL, l, ImVec2(stereoBarW, height));
        ImGui::SameLine(0.0f, stereoGap);
        DrawLevelBar(idR, r, ImVec2(stereoBarW, height));
        DrawDbScale(screen.x + barsWidth + 5.0f, screen.y, height);

        // Keep all captions inside the meter-panel child. The previous
        // meter height left too little room and clipped INPUT/OUTPUT.
        ImGui::SetCursorPosX(colStart + (colAvail - barsWidth) * 0.5f);
        ImGui::TextDisabled("L");
        ImGui::SameLine(stereoBarW + stereoGap + 4.0f);
        ImGui::TextDisabled("R");

        const ImVec2 ts = ImGui::CalcTextSize(name);
        ImGui::SetCursorPosX(colStart + (colAvail - ts.x) * 0.5f);
        ImGui::TextUnformatted(name);
    };

    stereoGroup("INPUT", "##input", inL, inR);

    ImGui::TableNextColumn();
    {
        const float colStart = ImGui::GetCursorPosX();
        const float colAvail = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(colStart + (colAvail - (grBarW + labelColumn)) * 0.5f);
        const ImVec2 screen = ImGui::GetCursorScreenPos();
        DrawGrBar("##gain_reduction", gr, ImVec2(grBarW, height));
        DrawDbScale(screen.x + grBarW + 5.0f, screen.y, height);

        const char* valueLabel = "GAIN REDUCTION";
        const ImVec2 ts = ImGui::CalcTextSize(valueLabel);
        ImGui::SetCursorPosX(colStart + (colAvail - ts.x) * 0.5f);
        ImGui::TextUnformatted(valueLabel);

        const float db = ImClamp(gr, 0.0f, kGrMaxDb);
        const char* fmt = db < 10.0f ? "%.1f dB" : "%.0f dB";
        ImGui::SetCursorPosX(colStart + (colAvail - 60.0f) * 0.5f);
        ImGui::TextDisabled(fmt, db);
    }

    stereoGroup("OUTPUT", "##output", outL, outR);
    ImGui::EndTable();
}

void DrawControls(ConfigValues& w, ConfigDocument& doc) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float mainKnob = avail > 420.0f ? 96.0f : 84.0f;
    const float smallKnob = avail > 420.0f ? 66.0f : 58.0f;

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.68f, 0.71f, 0.76f, 1.0f));
    ImGui::TextUnformatted("CONTROLS");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    {
        const float start = ImGui::GetCursorPosX();
        ImGui::SetCursorPosX(start + (avail - mainKnob) * 0.5f);
        float threshold = w.limiterThreshold;
        KnobState ks = { threshold, 0.1f, 1.0f, 0.95f,
                         "THRESHOLD", nullptr, mainKnob, 1.0f, LinearToDb };
        if (RotaryKnob(ks, "%.1f dB")) {
            w.limiterThreshold = ks.value;
            doc.MarkDirty();
            PushLiveFloat(svms::RLCommandType::SetLimiterThreshold, ks.value);
        }
    }

    ImGui::Spacing();

    if (ImGui::BeginTable("##limiter_timing", 3, ImGuiTableFlags_SizingStretchSame)) {
        auto timingKnob = [&](const char* label, float& value,
                              float minValue, float maxValue, float defaultValue,
                              svms::RLCommandType command) {
            ImGui::TableNextColumn();
            const float colStart = ImGui::GetCursorPosX();
            const float colAvail = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX(colStart + (colAvail - smallKnob) * 0.5f);
            KnobState ks = { value, minValue, maxValue, defaultValue,
                             label, "ms", smallKnob };
            if (RotaryKnob(ks, "%.1f ms")) {
                value = ks.value;
                doc.MarkDirty();
                PushLiveFloat(command, ks.value);
            }
        };

        timingKnob("LOOKAHEAD", w.limiterLookaheadMs, 0.0f, 20.0f, 3.0f,
                   svms::RLCommandType::SetLimiterLookahead);
        timingKnob("ATTACK", w.limiterAttackMs, 0.01f, 100.0f, 0.5f,
                   svms::RLCommandType::SetLimiterAttack);
        timingKnob("RELEASE", w.limiterReleaseMs, 1.0f, 5000.0f, 100.0f,
                   svms::RLCommandType::SetLimiterRelease);
        ImGui::EndTable();
    }
}

void DrawHistory(GrHistory& history, bool telemetryAvailable, float currentGr) {
    const float dt = ImGui::GetIO().DeltaTime;
    history.accumulator += dt;
    while (history.accumulator >= (1.0f / 30.0f)) {
        history.accumulator -= 1.0f / 30.0f;
        history.Push(telemetryAvailable ? currentGr : 0.0f);
    }

    const float graphW = ImGui::GetContentRegionAvail().x;
    const float graphH = (std::max)(150.0f,
        (std::min)(220.0f, ImGui::GetContentRegionAvail().y - 28.0f));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(p, ImVec2(p.x + graphW, p.y + graphH),
                      ImGui::GetColorU32(ImVec4(0.050f, 0.058f, 0.070f, 1.0f)), 5.0f);
    dl->AddRect(p, ImVec2(p.x + graphW, p.y + graphH),
                ImGui::GetColorU32(ImVec4(0.15f, 0.17f, 0.21f, 1.0f)), 5.0f);

    const float left = p.x + 42.0f;
    const float right = p.x + graphW - 10.0f;
    const float top = p.y + 24.0f;
    const float bottom = p.y + graphH - 24.0f;
    const float plotH = bottom - top;

    const float ticks[] = { 0.0f, 3.0f, 6.0f, 12.0f, 18.0f, 24.0f };
    for (float db : ticks) {
        const float y = top + plotH * (db / kGrMaxDb);
        dl->AddLine(ImVec2(left, y), ImVec2(right, y),
                    ImGui::GetColorU32(ImVec4(0.16f, 0.18f, 0.22f, 1.0f)), 1.0f);
        char label[16];
        std::snprintf(label, sizeof(label), db == 0.0f ? "0" : "-%.0f", db);
        dl->AddText(ImVec2(p.x + 7.0f, y - ImGui::GetFontSize() * 0.5f),
                    ImGui::GetColorU32(ImVec4(0.43f, 0.46f, 0.51f, 0.9f)), label);
    }

    dl->AddText(ImVec2(left, p.y + 5.0f),
                ImGui::GetColorU32(ImVec4(0.68f, 0.71f, 0.76f, 1.0f)),
                "GAIN REDUCTION HISTORY");

    if (telemetryAvailable && history.count >= 2) {
        ImVec2 previous{};
        bool havePrevious = false;
        for (int i = 0; i < history.count; ++i) {
            const float gr = history.AtOldestOffset(i);
            const float x = left + (right - left) *
                (static_cast<float>(i) / static_cast<float>(history.count - 1));
            const float y = top + plotH * (ImClamp(gr, 0.0f, kGrMaxDb) / kGrMaxDb);
            const ImVec2 point(x, y);
            if (havePrevious) {
                dl->AddLine(previous, point,
                            ImGui::GetColorU32(ImVec4(0.94f, 0.68f, 0.16f, 0.95f)), 2.0f);
                dl->AddQuadFilled(previous, point,
                                  ImVec2(point.x, top), ImVec2(previous.x, top),
                                  ImGui::GetColorU32(ImVec4(0.94f, 0.68f, 0.16f, 0.07f)));
            }
            previous = point;
            havePrevious = true;
        }

        char latest[32];
        std::snprintf(latest, sizeof(latest), "%.1f dB", currentGr);
        const ImVec2 ts = ImGui::CalcTextSize(latest);
        dl->AddText(ImVec2(right - ts.x, p.y + 5.0f),
                    ImGui::GetColorU32(ImVec4(0.94f, 0.68f, 0.16f, 1.0f)), latest);
    } else {
        const char* offline = telemetryAvailable
            ? "Waiting for limiter telemetry..."
            : "Runtime telemetry unavailable";
        const ImVec2 ts = ImGui::CalcTextSize(offline);
        dl->AddText(ImVec2((left + right - ts.x) * 0.5f,
                           (top + bottom - ts.y) * 0.5f),
                    ImGui::GetColorU32(ImVec4(0.42f, 0.45f, 0.50f, 1.0f)), offline);
    }

    dl->AddText(ImVec2(left, bottom + 5.0f),
                ImGui::GetColorU32(ImVec4(0.38f, 0.41f, 0.46f, 1.0f)), "10 s ago");
    const char* now = "now";
    const ImVec2 nowSize = ImGui::CalcTextSize(now);
    dl->AddText(ImVec2(right - nowSize.x, bottom + 5.0f),
                ImGui::GetColorU32(ImVec4(0.38f, 0.41f, 0.46f, 1.0f)), now);

    ImGui::Dummy(ImVec2(graphW, graphH));
}

} // namespace

void DrawLimiterPage(ConfigDocument& doc) {
    PushEffectPageStyle();

    auto& w = doc.Working();
    const auto& lc = GetLiveLinkContext();
    const auto* t = lc.telemetry;
    const bool telemetryAvailable = lc.connected && t != nullptr;
    static GrHistory history;

    if (ImGui::BeginTable("##limiter_header", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("enable", ImGuiTableColumnFlags_WidthFixed, 155.0f);
        ImGui::TableSetupColumn("title", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthFixed, 190.0f);
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        bool enabled = w.limiterEnabled;
        if (ToggleSwitch("ENABLED", &enabled, "Enable the transparent brick-wall limiter.")) {
            w.limiterEnabled = enabled;
            doc.MarkDirty();
            PushLiveBool(svms::RLCommandType::SetLimiterEnabled, enabled);
        }

        ImGui::TableNextColumn();
        const char* title = "LIMITER";
        const ImVec2 titleSize = ImGui::CalcTextSize(title);
        const float start = ImGui::GetCursorPosX();
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(start + (width - titleSize.x) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.75f, 0.55f, 1.0f));
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();

        ImGui::TableNextColumn();
        if (lc.connected) LiveBadge("All limiter parameters are live-previewed");
        AppliedStateBadge(lc.connected, lc.telemetry, w,
                          "Limiter group applied state vs working copy");
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::Spacing();

    const float inL = telemetryAvailable ? t->limiterInputPeakL : 0.0f;
    const float inR = telemetryAvailable ? t->limiterInputPeakR : 0.0f;
    const float outL = telemetryAvailable ? t->limiterOutputPeakL : 0.0f;
    const float outR = telemetryAvailable ? t->limiterOutputPeakR : 0.0f;
    const float gr = telemetryAvailable ? t->limiterGainReductionDb : 0.0f;

    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float topHeight = (std::max)(285.0f,
        (std::min)(360.0f, ImGui::GetContentRegionAvail().y * 0.52f));

    if (ImGui::BeginTable("##limiter_top", 2,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("meters", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("controls", ImGuiTableColumnFlags_WidthStretch,
                                availableWidth > 900.0f ? 1.10f : 0.95f);
        ImGui::TableNextRow(ImGuiTableRowFlags_None, topHeight);

        ImGui::TableNextColumn();
        ImGui::BeginChild("##meter_panel", ImVec2(0.0f, topHeight - 6.0f), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::TextDisabled("METERS");
        ImGui::Spacing();
        // Leave enough vertical room for L/R, INPUT/OUTPUT and GR captions.
        // At the old -78 offset the final line intersected the child clip rect.
        const float meterHeight = (std::max)(140.0f, topHeight - 102.0f);
        DrawMeterBank(inL, inR, gr, outL, outR, meterHeight);
        ImGui::EndChild();

        ImGui::TableNextColumn();
        ImGui::BeginChild("##control_panel", ImVec2(0.0f, topHeight - 6.0f), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawControls(w, doc);
        ImGui::EndChild();
        ImGui::EndTable();
    }

    ImGui::Spacing();

    if (telemetryAvailable) {
        ImGui::TextDisabled("IN  L %.1f / R %.1f dB     OUT  L %.1f / R %.1f dB     GR %.1f dB",
                            LinearToDb(inL), LinearToDb(inR),
                            LinearToDb(outL), LinearToDb(outR), gr);
    } else {
        ImGui::TextDisabled("Runtime telemetry unavailable — controls remain editable and live preview resumes when a driver connects.");
    }

    ImGui::Spacing();
    DrawHistory(history, telemetryAvailable, gr);

    PopEffectPageStyle();
}

} // namespace svms::cfg
