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
constexpr float kGrBaseMaxDb = 24.0f;
constexpr float kGrHardMaxDb = 240.0f;
constexpr int kGrHistoryCapacity = 320; // ~10.7 s at 30 Hz

float LinearToDb(float linear) {
    if (!std::isfinite(linear) || linear <= 0.000001f) return kMeterFloorDb;
    return (std::max)(kMeterFloorDb, 20.0f * std::log10(linear));
}

float MeterNorm(float linear) {
    const float db = LinearToDb(linear);
    return ImClamp((db - kMeterFloorDb) / -kMeterFloorDb, 0.0f, 1.0f);
}

float SelectGrDisplayMax(float observedDb) {
    if (!std::isfinite(observedDb)) return kGrBaseMaxDb;
    observedDb = ImClamp(observedDb, 0.0f, kGrHardMaxDb);
    if (observedDb <= kGrBaseMaxDb) return kGrBaseMaxDb;
    const float steps = std::ceil(observedDb / kGrBaseMaxDb);
    return ImClamp(steps * kGrBaseMaxDb, kGrBaseMaxDb, kGrHardMaxDb);
}

float GrTickStep(float displayMaxDb) {
    if (displayMaxDb <= 24.0f) return 6.0f;
    if (displayMaxDb <= 72.0f) return 12.0f;
    if (displayMaxDb <= 144.0f) return 24.0f;
    return 48.0f;
}

void DrawCenteredText(ImDrawList* dl, float centerX, float y,
                      ImU32 color, const char* text) {
    const ImVec2 size = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(centerX - size.x * 0.5f, y), color, text);
}

struct GrHistory {
    float values[kGrHistoryCapacity]{};
    int writePos = 0;
    int count = 0;
    float accumulator = 0.0f;

    void Push(float value) {
        if (!std::isfinite(value)) value = 0.0f;
        values[writePos] = ImClamp(value, 0.0f, kGrHardMaxDb);
        writePos = (writePos + 1) % kGrHistoryCapacity;
        if (count < kGrHistoryCapacity) ++count;
    }

    float AtOldestOffset(int i) const {
        const int start = (writePos - count + kGrHistoryCapacity) % kGrHistoryCapacity;
        return values[(start + i) % kGrHistoryCapacity];
    }

    float Maximum() const {
        float maximum = 0.0f;
        for (int i = 0; i < count; ++i)
            maximum = (std::max)(maximum, AtOldestOffset(i));
        return maximum;
    }
};

struct MeterVisualState {
    float inL = 0.0f;
    float inR = 0.0f;
    float outL = 0.0f;
    float outR = 0.0f;
    float gr = 0.0f;
    bool initialized = false;
};

float SmoothVisual(float current, float target, float dt,
                   float riseSeconds, float fallSeconds) {
    if (!std::isfinite(target)) target = 0.0f;
    const float tau = target > current ? riseSeconds : fallSeconds;
    const float safeTau = (std::max)(0.001f, tau);
    const float alpha = 1.0f - std::exp(-dt / safeTau);
    return current + (target - current) * ImClamp(alpha, 0.0f, 1.0f);
}

void UpdateMeterVisualState(MeterVisualState& state, bool telemetryAvailable,
                            float inL, float inR, float gr,
                            float outL, float outR) {
    const float dt = ImClamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);

    if (telemetryAvailable && !state.initialized) {
        state.inL = inL;
        state.inR = inR;
        state.outL = outL;
        state.outR = outR;
        state.gr = gr;
        state.initialized = true;
        return;
    }

    const float targetInL = telemetryAvailable ? inL : 0.0f;
    const float targetInR = telemetryAvailable ? inR : 0.0f;
    const float targetOutL = telemetryAvailable ? outL : 0.0f;
    const float targetOutR = telemetryAvailable ? outR : 0.0f;
    const float targetGr = telemetryAvailable ? gr : 0.0f;

    state.inL = SmoothVisual(state.inL, targetInL, dt, 0.012f, 0.120f);
    state.inR = SmoothVisual(state.inR, targetInR, dt, 0.012f, 0.120f);
    state.outL = SmoothVisual(state.outL, targetOutL, dt, 0.012f, 0.120f);
    state.outR = SmoothVisual(state.outR, targetOutR, dt, 0.012f, 0.120f);
    state.gr = SmoothVisual(state.gr, targetGr, dt, 0.020f, 0.140f);
}

void DrawLevelBar(const char* id, float linear, const ImVec2& size) {
    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    DrawLedLadder(ImGui::GetWindowDrawList(), pos, size, MeterNorm(linear),
                  0.0f, false, nullptr);
    ImGui::Dummy(size);
    ImGui::PopID();
}

void DrawGrBar(const char* id, float reductionDb, float displayMaxDb,
               const ImVec2& size) {
    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    displayMaxDb = (std::max)(kGrBaseMaxDb, displayMaxDb);
    const ImVec4 amber = GetAccent();
    DrawLedLadder(ImGui::GetWindowDrawList(), pos, size,
                  ImClamp(reductionDb / displayMaxDb, 0.0f, 1.0f), 0.0f, true,
                  &amber);
    ImGui::Dummy(size);
    ImGui::PopID();
}

void DrawDbScale(float x, float top, float height) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    constexpr int tickCount = 7;
    const float ticks[tickCount] = {
        0.0f, -3.0f, -6.0f, -12.0f, -24.0f, -48.0f, -60.0f
    };

    const float fontH = ImGui::GetFontSize();
    const float minGap = fontH + 2.0f;
    const float minCenter = top + fontH * 0.5f;
    const float maxCenter = top + height - fontH * 0.5f;

    float desired[tickCount]{};
    float placed[tickCount]{};
    bool visible[tickCount]{};

    int maxLabels = static_cast<int>(height / minGap) + 1;
    maxLabels = (std::max)(2, (std::min)(tickCount, maxLabels));

    for (int i = 0; i < tickCount; ++i) visible[i] = true;
    if (maxLabels < 7) visible[1] = false;
    if (maxLabels < 6) visible[5] = false;
    if (maxLabels < 5) visible[2] = false;
    if (maxLabels < 4) visible[4] = false;
    if (maxLabels < 3) visible[3] = false;

    int indices[tickCount]{};
    int count = 0;
    for (int i = 0; i < tickCount; ++i) {
        if (!visible[i]) continue;
        const float norm = (ticks[i] - kMeterFloorDb) / -kMeterFloorDb;
        desired[i] = top + height * (1.0f - norm);
        desired[i] = ImClamp(desired[i], minCenter, maxCenter);
        placed[i] = desired[i];
        indices[count++] = i;
    }

    for (int n = 1; n < count; ++n) {
        const int prev = indices[n - 1];
        const int cur = indices[n];
        placed[cur] = (std::max)(placed[cur], placed[prev] + minGap);
    }
    if (count > 0) {
        const int last = indices[count - 1];
        placed[last] = (std::min)(placed[last], maxCenter);
    }
    for (int n = count - 2; n >= 0; --n) {
        const int cur = indices[n];
        const int next = indices[n + 1];
        placed[cur] = (std::min)(placed[cur], placed[next] - minGap);
    }
    if (count > 0) {
        const int first = indices[0];
        if (placed[first] < minCenter) {
            const float offset = minCenter - placed[first];
            for (int n = 0; n < count; ++n) placed[indices[n]] += offset;
        }
    }

    const ImU32 textColor = ImGui::GetColorU32(
        ImVec4(0.43f, 0.46f, 0.51f, 0.9f));
    const ImU32 leaderColor = ImGui::GetColorU32(
        ImVec4(0.30f, 0.33f, 0.38f, 0.72f));

    for (int n = 0; n < count; ++n) {
        const int i = indices[n];
        char text[16];
        std::snprintf(text, sizeof(text), "%.0f", ticks[i]);

        const float labelCenter = placed[i];
        const float labelY = labelCenter - fontH * 0.5f;
        const float exactY = desired[i];

        dl->AddLine(ImVec2(x - 5.0f, exactY), ImVec2(x - 1.0f, exactY),
                    leaderColor, 1.0f);
        if (std::fabs(labelCenter - exactY) > 0.75f) {
            dl->AddLine(ImVec2(x - 1.0f, exactY), ImVec2(x + 2.0f, labelCenter),
                        leaderColor, 1.0f);
        }
        dl->AddText(ImVec2(x + 4.0f, labelY), textColor, text);
    }
}

void DrawGrScale(float x, float top, float height, float currentGr,
                 float displayMaxDb) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    displayMaxDb = ImClamp(displayMaxDb, kGrBaseMaxDb, kGrHardMaxDb);
    const float tickStep = GrTickStep(displayMaxDb);

    float ticks[16]{};
    int tickCount = 0;
    for (float db = 0.0f;
         db < displayMaxDb - 0.01f && tickCount < 15;
         db += tickStep) {
        ticks[tickCount++] = db;
    }
    ticks[tickCount++] = displayMaxDb;

    const float fontH = ImGui::GetFontSize();
    const float minGap = fontH + 2.0f;
    const float minCenter = top + fontH * 0.5f;
    const float maxCenter = top + height - fontH * 0.5f;
    const float live = ImClamp(currentGr, 0.0f, displayMaxDb);
    const float liveExactY = top + height * (live / displayMaxDb);
    const float liveCenter = ImClamp(liveExactY, minCenter, maxCenter);

    const ImU32 textColor = ImGui::GetColorU32(
        ImVec4(0.43f, 0.46f, 0.51f, 0.9f));
    const ImU32 leaderColor = ImGui::GetColorU32(
        ImVec4(0.30f, 0.33f, 0.38f, 0.72f));
    const ImU32 liveColor = ImGui::GetColorU32(
        ImVec4(0.94f, 0.68f, 0.16f, 1.0f));

    float desired[16]{};
    bool visible[16]{};
    for (int i = 0; i < tickCount; ++i) {
        desired[i] = top + height * (ticks[i] / displayMaxDb);
        desired[i] = ImClamp(desired[i], minCenter, maxCenter);
        visible[i] = true;

        if (std::fabs(desired[i] - liveCenter) < minGap * 0.92f &&
            std::fabs(ticks[i] - live) > 0.05f) {
            visible[i] = false;
        }
    }

    for (int i = 0; i < tickCount; ++i) {
        if (std::fabs(ticks[i] - live) <= 0.05f) visible[i] = false;
    }

    for (int i = 0; i < tickCount; ++i) {
        dl->AddLine(ImVec2(x - 5.0f, desired[i]), ImVec2(x - 1.0f, desired[i]),
                    leaderColor, 1.0f);
        if (!visible[i]) continue;

        char text[16];
        std::snprintf(text, sizeof(text), "%.0f", ticks[i]);
        dl->AddText(ImVec2(x + 4.0f, desired[i] - fontH * 0.5f), textColor, text);
    }

    char liveText[16];
    const float liveValue = (std::max)(0.0f,
        std::isfinite(currentGr) ? currentGr : 0.0f);
    std::snprintf(liveText, sizeof(liveText),
                  liveValue < 10.0f ? "%.1f" : "%.0f", liveValue);
    dl->AddLine(ImVec2(x - 8.0f, liveExactY), ImVec2(x + 1.0f, liveExactY),
                liveColor, 1.5f);
    dl->AddText(ImVec2(x + 4.0f, liveCenter - fontH * 0.5f), liveColor, liveText);
}

void DrawMeterBank(float inL, float inR, float gr, float outL, float outR,
                   float height, float grDisplayMaxDb) {
    const float scaleGap = 5.0f;
    const float scaleWidth = ImGui::CalcTextSize("-240").x + 8.0f;
    const float stereoGap = 6.0f;
    const float captionGap = 5.0f;
    const float captionHeight = ImGui::GetFontSize() * 2.0f + captionGap + 4.0f;
    const ImU32 captionColor = ImGui::GetColorU32(ImVec4(0.56f, 0.59f, 0.62f, 1.0f));
    const ImU32 mainCaptionColor = ImGui::GetColorU32(ImVec4(0.82f, 0.84f, 0.87f, 1.0f));

    if (!ImGui::BeginTable("##meter_bank", 3, ImGuiTableFlags_SizingStretchSame)) return;

    auto stereoGroup = [&](const char* name, const char* idPrefix, float l, float r) {
        ImGui::TableNextColumn();
        const float colStart = ImGui::GetCursorPosX();
        const float colAvail = ImGui::GetContentRegionAvail().x;

        const float maxPairW = (std::max)(52.0f,
            colAvail - scaleGap - scaleWidth - 12.0f);
        const float pairW = (std::min)(112.0f, maxPairW);
        const float barW = (std::max)(20.0f, (pairW - stereoGap) * 0.5f);
        const float barsWidth = barW * 2.0f + stereoGap;
        const float visualWidth = barsWidth + scaleGap + scaleWidth;
        const float groupX = colStart + (colAvail - visualWidth) * 0.5f;

        ImGui::SetCursorPosX(groupX);
        const ImVec2 screen = ImGui::GetCursorScreenPos();

        char idL[32], idR[32];
        std::snprintf(idL, sizeof(idL), "%sL", idPrefix);
        std::snprintf(idR, sizeof(idR), "%sR", idPrefix);
        DrawLevelBar(idL, l, ImVec2(barW, height));
        ImGui::SameLine(0.0f, stereoGap);
        DrawLevelBar(idR, r, ImVec2(barW, height));
        DrawDbScale(screen.x + barsWidth + scaleGap, screen.y, height);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float labelsY = screen.y + height + 4.0f;
        DrawCenteredText(dl, screen.x + barW * 0.5f,
                         labelsY, captionColor, "L");
        DrawCenteredText(dl, screen.x + barW + stereoGap + barW * 0.5f,
                         labelsY, captionColor, "R");
        DrawCenteredText(dl, screen.x + barsWidth * 0.5f,
                         labelsY + ImGui::GetFontSize() + captionGap,
                         mainCaptionColor, name);

        ImGui::SetCursorPosX(colStart);
        ImGui::Dummy(ImVec2(1.0f, captionHeight));
    };

    stereoGroup("INPUT", "##input", inL, inR);

    ImGui::TableNextColumn();
    {
        const float colStart = ImGui::GetCursorPosX();
        const float colAvail = ImGui::GetContentRegionAvail().x;
        const float nameWidth = ImGui::CalcTextSize("GAIN REDUCTION").x;
        const float maxBarW = (std::max)(44.0f,
            colAvail - scaleGap - scaleWidth - 12.0f);
        const float desiredBarW = (std::max)(72.0f, nameWidth + 12.0f);
        const float barW = (std::min)(desiredBarW, maxBarW);
        const float visualWidth = barW + scaleGap + scaleWidth;
        const float groupX = colStart + (colAvail - visualWidth) * 0.5f;

        ImGui::SetCursorPosX(groupX);
        const ImVec2 screen = ImGui::GetCursorScreenPos();
        DrawGrBar("##gain_reduction", gr, grDisplayMaxDb,
                  ImVec2(barW, height));
        DrawGrScale(screen.x + barW + scaleGap, screen.y, height, gr,
                    grDisplayMaxDb);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float centerX = screen.x + barW * 0.5f;
        const float labelsY = screen.y + height + 4.0f;
        DrawCenteredText(dl, centerX, labelsY,
                         mainCaptionColor, "GAIN REDUCTION");

        const float db = (std::max)(0.0f,
            std::isfinite(gr) ? gr : 0.0f);
        char dbText[32];
        std::snprintf(dbText, sizeof(dbText), db < 10.0f ? "%.1f dB" : "%.0f dB", db);
        DrawCenteredText(dl, centerX,
                         labelsY + ImGui::GetFontSize() + captionGap,
                         captionColor, dbText);

        ImGui::SetCursorPosX(colStart);
        ImGui::Dummy(ImVec2(1.0f, captionHeight));
    }

    stereoGroup("OUTPUT", "##output", outL, outR);
    ImGui::EndTable();
}

bool DrawHeaderToggle(const char* id, const char* label, bool* value,
                      const char* tooltip) {
    ImGui::PushID(id);
    const float switchW = 44.0f;
    const float switchH = 22.0f;
    const float gap = 10.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const ImVec2 itemSize(switchW + gap + textSize.x, switchH);

    ImGui::InvisibleButton("##header_toggle", itemSize);
    bool changed = false;
    if (ImGui::IsItemClicked()) {
        *value = !*value;
        changed = true;
    }
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ThemeSettings& theme = GetThemeSettings();
    const float radius = switchH * 0.5f;
    const float t = *value ? 1.0f : 0.0f;
    const float cx = pos.x + radius + t * (switchW - radius * 2.0f);
    const float cy = pos.y + radius;
    dl->AddRectFilled(pos, ImVec2(pos.x + switchW, pos.y + switchH),
                      ImGui::GetColorU32(*value ? GetAccent() : theme.control),
                      radius);
    dl->AddCircleFilled(ImVec2(cx, cy), radius - 2.0f,
                        ImGui::GetColorU32(theme.text));
    dl->AddText(ImVec2(pos.x + switchW + gap,
                       pos.y + (switchH - textSize.y) * 0.5f),
                ImGui::GetColorU32(theme.text), label);

    if (tooltip && hovered) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tooltip);
        ImGui::EndTooltip();
    }
    ImGui::PopID();
    return changed;
}

void DrawHeaderState(bool connected,
                     const svms::RuntimeLinkTelemetryV2* telemetry,
                     const ConfigValues& values,
                     const char* liveTooltip,
                     const char* appliedTooltip) {
    const bool synced = connected && telemetry &&
                        LiveAppliedMatches(*telemetry, values);
    const char* stateText = !connected || !telemetry
        ? "OFFLINE" : (synced ? "APPLIED" : "PENDING");
    const ImVec4 stateColor = !connected || !telemetry
        ? GetMutedText() : (synced ? GetSuccess() : GetWarning());
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    float totalWidth = ImGui::CalcTextSize(stateText).x;
    if (connected) totalWidth += ImGui::CalcTextSize("LIVE").x + spacing;

    const float startX = ImGui::GetCursorPosX();
    const float available = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(startX + (std::max)(0.0f, available - totalWidth));

    if (connected) {
        ImGui::PushStyleColor(ImGuiCol_Text, GetSuccess());
        ImGui::TextUnformatted("LIVE");
        ImGui::PopStyleColor();
        if (liveTooltip && ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(liveTooltip);
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
    }

    ImGui::PushStyleColor(ImGuiCol_Text, stateColor);
    ImGui::TextUnformatted(stateText);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(appliedTooltip);
        if (connected && telemetry && !synced)
            ImGui::TextUnformatted("Working values differ from the engine's applied echo.");
        ImGui::EndTooltip();
    }
}

void DrawControls(ConfigValues& w, ConfigDocument& doc,
                  bool connected, const svms::RuntimeLinkTelemetryV2* telemetry) {
    if (!BeginRackPanel("LIMITER")) { EndRackPanel(); return; }
    const float avail = ImGui::GetContentRegionAvail().x;

    // Row: enable lever, algorithm display, applied state.
    bool enabled = w.limiterEnabled;
    if (ToggleSwitch("ENABLED", &enabled, "Enable the transparent brick-wall limiter.")) {
        w.limiterEnabled = enabled;
        doc.MarkDirty();
        PushLiveBool(svms::RLCommandType::SetLimiterEnabled, enabled);
    }
    ImGui::SameLine(0.0f, 28.0f);
    {
        static const char* algorithms[] = {"Classic", "Adaptive (Experimental)"};
        int algorithm = static_cast<int>((std::min)(1u, w.limiterAlgorithm));
        ImGui::SetNextItemWidth(220.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, GetAccent());
        if (ImGui::Combo("##limiter_algorithm", &algorithm, algorithms, 2)) {
            w.limiterAlgorithm = static_cast<uint32_t>(algorithm);
            doc.MarkDirty();
            PushLiveLimiterAlgorithm(w.limiterAlgorithm);
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(w.limiterAlgorithm == 0u
                ? "Original SVMS limiter used by v0.6.5 and earlier."
                : "Predictive lookahead limiter with program-dependent release.");
            ImGui::EndTooltip();
        }
    }
    ImGui::SameLine(0.0f, 28.0f);
    {
        const float stateY = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(stateY + 4.0f);
        const bool synced = connected && telemetry &&
                            LiveAppliedMatches(*telemetry, w);
        const char* text = !connected || !telemetry
            ? "OFFLINE" : (synced ? "APPLIED" : "PENDING");
        const ImVec4 color = !connected || !telemetry
            ? GetMutedText() : (synced ? GetSuccess() : GetWarning());
        PushMono();
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
        PopMono();
    }
    ImGui::Spacing();
    ImGui::Spacing();

    const float knob = avail > 700.0f ? 66.0f : 56.0f;
    if (ImGui::BeginTable("##limiter_knobs", 4, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        auto place = [&](const char* label, float& value, float minV, float maxV,
                         float defV, const char* fmt, svms::RLCommandType cmd,
                         float (*displayFn)(float)) {
            ImGui::TableNextColumn();
            const float start = ImGui::GetCursorPosX();
            const float colAvail = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX(start + (colAvail - knob) * 0.5f);
            KnobState ks = {value, minV, maxV, defV, label, nullptr, knob, 1.0f, displayFn};
            if (RotaryKnob(ks, fmt)) {
                value = ks.value;
                doc.MarkDirty();
                PushLiveFloat(cmd, ks.value);
            }
        };
        place("THRESHOLD", w.limiterThreshold, 0.1f, 1.0f, 0.95f, "%.1f dB",
              svms::RLCommandType::SetLimiterThreshold, LinearToDb);
        place("LOOKAHEAD", w.limiterLookaheadMs, 0.0f, 20.0f, 3.0f, "%.1f ms",
              svms::RLCommandType::SetLimiterLookahead, nullptr);
        place("ATTACK", w.limiterAttackMs, 0.01f, 100.0f, 0.5f, "%.1f ms",
              svms::RLCommandType::SetLimiterAttack, nullptr);
        place("RELEASE", w.limiterReleaseMs, 1.0f, 5000.0f, 100.0f, "%.1f ms",
              svms::RLCommandType::SetLimiterRelease, nullptr);
        ImGui::EndTable();
    }
    EndRackPanel();
}

void DrawHistory(GrHistory& history, bool telemetryAvailable, float currentGr,
                 float grDisplayMaxDb) {
    const float dt = ImGui::GetIO().DeltaTime;
    history.accumulator += dt;
    while (history.accumulator >= (1.0f / 30.0f)) {
        history.accumulator -= 1.0f / 30.0f;
        history.Push(telemetryAvailable ? currentGr : 0.0f);
    }

    const float graphW = ImGui::GetContentRegionAvail().x;
    const float graphH = 196.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    DrawLcdFrame(dl, p, ImVec2(p.x + graphW, p.y + graphH));
    const ImVec4 acc = GetAccent();
    const ImVec4 mut = GetMutedText();
    auto wa = [&](float a) { ImVec4 c = acc; c.w = a; return c; };
    auto wm = [&](float a) { ImVec4 c = mut; c.w = a; return c; };
    PushMono();

    const float left = p.x + 48.0f;
    const float right = p.x + graphW - 10.0f;
    const float top = p.y + 24.0f;
    const float bottom = p.y + graphH - 24.0f;
    const float plotH = bottom - top;

    grDisplayMaxDb = ImClamp(grDisplayMaxDb, kGrBaseMaxDb, kGrHardMaxDb);
    const float tickStep = GrTickStep(grDisplayMaxDb);
    auto drawHistoryTick = [&](float db) {
        const float y = top + plotH * (db / grDisplayMaxDb);
        dl->AddLine(ImVec2(left, y), ImVec2(right, y),
                    ImGui::GetColorU32(wa(0.13f)), 1.0f);
        char label[16];
        std::snprintf(label, sizeof(label), db == 0.0f ? "0" : "-%.0f", db);
        dl->AddText(ImVec2(p.x + 7.0f, y - ImGui::GetFontSize() * 0.5f),
                    ImGui::GetColorU32(wm(0.9f)), label);
    };
    for (float db = 0.0f; db < grDisplayMaxDb - 0.01f; db += tickStep)
        drawHistoryTick(db);
    drawHistoryTick(grDisplayMaxDb);

    dl->AddText(ImVec2(left, p.y + 5.0f),
                ImGui::GetColorU32(wm(1.0f)),
                "GAIN REDUCTION HISTORY");

    if (telemetryAvailable && history.count >= 2) {
        ImVec2 previous{};
        bool havePrevious = false;
        for (int i = 0; i < history.count; ++i) {
            const float histGr = history.AtOldestOffset(i);
            const float x = left + (right - left) *
                (static_cast<float>(i) / static_cast<float>(history.count - 1));
            const float y = top + plotH *
                (ImClamp(histGr, 0.0f, grDisplayMaxDb) / grDisplayMaxDb);
            const ImVec2 point(x, y);
            if (havePrevious) {
                dl->AddLine(previous, point,
                            ImGui::GetColorU32(wa(0.95f)), 2.0f);
                dl->AddQuadFilled(previous, point,
                                  ImVec2(point.x, top), ImVec2(previous.x, top),
                                  ImGui::GetColorU32(wa(0.07f)));
            }
            previous = point;
            havePrevious = true;
        }

        char latest[32];
        std::snprintf(latest, sizeof(latest), "%.1f dB", currentGr);
        const ImVec2 ts = ImGui::CalcTextSize(latest);
        dl->AddText(ImVec2(right - ts.x, p.y + 5.0f),
                    ImGui::GetColorU32(wa(1.0f)), latest);
    } else {
        const char* offline = telemetryAvailable
            ? "Waiting for limiter telemetry..."
            : "Runtime telemetry unavailable";
        const ImVec2 ts = ImGui::CalcTextSize(offline);
        dl->AddText(ImVec2((left + right - ts.x) * 0.5f,
                           (top + bottom - ts.y) * 0.5f),
                    ImGui::GetColorU32(wm(1.0f)), offline);
    }

    dl->AddText(ImVec2(left, bottom + 5.0f),
                ImGui::GetColorU32(wm(1.0f)), "10 s ago");
    const char* now = "now";
    const ImVec2 nowSize = ImGui::CalcTextSize(now);
    dl->AddText(ImVec2(right - nowSize.x, bottom + 5.0f),
                ImGui::GetColorU32(wm(1.0f)), now);

    PopMono();
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
    static MeterVisualState meterVisuals;

    const float inL = telemetryAvailable ? t->limiterInputPeakL : 0.0f;
    const float inR = telemetryAvailable ? t->limiterInputPeakR : 0.0f;
    const float outL = telemetryAvailable ? t->limiterOutputPeakL : 0.0f;
    const float outR = telemetryAvailable ? t->limiterOutputPeakR : 0.0f;
    const float gr = telemetryAvailable ? t->limiterGainReductionDb : 0.0f;

    UpdateMeterVisualState(meterVisuals, telemetryAvailable,
                           inL, inR, gr, outL, outR);
    const float grDisplayMaxDb = SelectGrDisplayMax((std::max)(
        gr, (std::max)(meterVisuals.gr, history.Maximum())));

    if (ImGui::BeginTable("##limiter_top", 2,
                          ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("meters", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("history", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        if (BeginRackPanel("METERS")) {
            DrawMeterBank(meterVisuals.inL, meterVisuals.inR, meterVisuals.gr,
                          meterVisuals.outL, meterVisuals.outR, 150.0f,
                          grDisplayMaxDb);
        }
        EndRackPanel();

        ImGui::TableNextColumn();
        if (BeginRackPanel("GAIN REDUCTION  10 S")) {
            DrawHistory(history, telemetryAvailable, gr, grDisplayMaxDb);
        }
        EndRackPanel();
        ImGui::EndTable();
    }

    DrawControls(w, doc, lc.connected, lc.telemetry);

    PushMono();
    if (telemetryAvailable) {
        ImGui::TextDisabled("IN  L %.1f / R %.1f dB     OUT  L %.1f / R %.1f dB     GR %.1f dB",
                            LinearToDb(inL), LinearToDb(inR),
                            LinearToDb(outL), LinearToDb(outR), gr);
    } else {
        ImGui::TextDisabled("TELEMETRY OFFLINE - controls stay editable; live preview resumes when a driver connects.");
    }
    PopMono();

    PopEffectPageStyle();
}

} // namespace svms::cfg
