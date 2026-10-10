#include "PagePerformance.h"
#include "ConfigDocument.h"
#include "Widgets.h"
#include "Theme.h"
#include "imgui.h"
#include "../SVMSRuntimeLinkProtocol.h"
#include "../SVMSRuntimeLink.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>

namespace svms::cfg {
namespace {

bool BeginSettingsTable(const char* id) {
    if (!ImGui::BeginTable(id, 3,
                           ImGuiTableFlags_SizingStretchProp |
                           ImGuiTableFlags_BordersInnerH |
                           ImGuiTableFlags_RowBg,
                           ImVec2(0.0f, 0.0f))) {
        return false;
    }
    ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthFixed, 250.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 135.0f);
    return true;
}

void LabelCell(const char* label, const char* tooltip = nullptr) {
    ImGui::TableNextColumn();
    SettingLabel(label, tooltip);
}

void CenteredStatusCell(const char* label, const ImVec4& color,
                        const char* tooltip) {
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    const float startX = ImGui::GetCursorPosX();
    const float available = ImGui::GetContentRegionAvail().x;
    const float labelWidth = ImGui::CalcTextSize(label).x;
    ImGui::SetCursorPosX(startX + (std::max)(0.0f, (available - labelWidth) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
        ImGui::TextUnformatted(tooltip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void RestartCell() {
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    PushMono(0.78f);
    const float pillW = ImGui::CalcTextSize("RESTART").x + 12.0f;
    PopMono();
    const float startX = ImGui::GetCursorPosX();
    const float available = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(startX + (std::max)(0.0f, (available - pillW) * 0.5f));
    RestartPill();
}

void LiveVoiceCell() {
    CenteredStatusCell(
        "LIVE", GetSuccess(),
        "Applies live. Voice capacity changes allocate or shed at a render boundary while preserving sounding voices; the retire floor applies to newly started releases without touching a sounding tail.");
}

float BlockBudgetMs(const svms::RuntimeLinkTelemetryV2& t) {
    if (t.sampleRate == 0u) return 0.0f;
    return static_cast<float>(t.bufferFrames) * 1000.0f /
           static_cast<float>(t.sampleRate);
}

float PercentToMs(float percent, float budgetMs) {
    return budgetMs * percent * 0.01f;
}

void BudgetMetric(const char* label, const char* format, ...) {
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    va_list args;
    va_start(args, format);
    ImGui::TextV(format, args);
    va_end(args);
}

} // namespace

void DrawPerformancePage(ConfigDocument& doc) {
    auto& w = doc.Working();
    const auto& lc = GetLiveLinkContext();
    const auto* t = lc.telemetry;

    auto send = [&](svms::RLCommandType type, uint32_t param) {
        if (!lc.connected || !lc.client) return;
        char result[svms::kRuntimeLinkResultTextCapacity]{};
        lc.client->SendCommand(type, 0u, param, svms::RuntimeLiveStateV2{}, 100u, result);
    };

    if (ImGui::BeginTable("##synth_top", 2,
                          ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("voices", ImGuiTableColumnFlags_WidthStretch, 1.35f);
        ImGui::TableSetupColumn("engine", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableNextRow();

        // ------------------------------------------------------- VOICES
        ImGui::TableNextColumn();
        if (BeginRackPanel("VOICES")) {
            const ThemeSettings& th = GetThemeSettings();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            // Big readout of the live voice ceiling.
            {
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                const float width = (std::min)(260.0f, ImGui::GetContentRegionAvail().x * 0.5f);
                const float height = 64.0f;
                DrawLcdFrame(dl, p0, ImVec2(p0.x + width, p0.y + height));
                PushMono(0.80f);
                dl->AddText(ImVec2(p0.x + 10.0f, p0.y + 6.0f),
                            ImGui::GetColorU32(ImVec4(th.accent.x, th.accent.y, th.accent.z, 0.5f)),
                            "MAX VOICES");
                PopMono();
                PushMono(2.1f);
                char big[32];
                std::snprintf(big, sizeof(big), "%u", w.maxVoices);
                dl->AddText(ImVec2(p0.x + 10.0f, p0.y + 20.0f),
                            ImGui::GetColorU32(th.accent), big);
                PopMono();
                ImGui::Dummy(ImVec2(width, height));
                if (t && lc.connected) {
                    ImGui::SameLine(0.0f, 20.0f);
                    ImGui::BeginGroup();
                    PanelCaption("ACTIVE NOW", nullptr);
                    PushMono(1.3f);
                    ImGui::PushStyleColor(ImGuiCol_Text, th.accent);
                    ImGui::Text("%u", t->activeVoices);
                    ImGui::PopStyleColor();
                    PopMono();
                    ImGui::EndGroup();
                }
            }
            ImGui::Spacing();

            static const int presetValues[] = {1024, 2048, 4096, 8192, 16384, 32768,
                                               65536, 131072, 262144, 524288};
            static const char* presetLabels[] = {"1k", "2k", "4k", "8k", "16k", "32k",
                                                 "64k", "128k", "256k", "512k"};
            int presetIdx = -1;
            for (int i = 0; i < 10; ++i)
                if (presetValues[i] == static_cast<int>(w.maxVoices)) presetIdx = i;
            if (PanelKeys("VOICE CEILING  (LIVE)", &presetIdx, presetLabels, 10,
                          "Live primary-voice ceiling. Growing the value expands the physical voice pool without restarting or resetting sounding voices. Lowering it sheds excess voices using the normal steal-priority policy and a short anti-click release.") &&
                presetIdx >= 0) {
                w.maxVoices = static_cast<uint32_t>(presetValues[presetIdx]);
                doc.MarkDirty();
                PushLiveMaxVoices(w.maxVoices);
            }
            ImGui::Spacing();

            ImGui::BeginGroup();
            int custom = static_cast<int>(w.maxVoices);
            bool done = false;
            if (PanelLcdInt("CUSTOM CEILING", &custom, 1, 524288,
                            "Type an exact voice count. Applied when you finish editing, so deleting a digit never sheds thousands of voices mid-edit.",
                            false, &done)) {
                w.maxVoices = static_cast<uint32_t>(custom);
                doc.MarkDirty();
            }
            if (done) PushLiveMaxVoices(w.maxVoices);
            ImGui::EndGroup();
            ImGui::SameLine(0.0f, 28.0f);
            ImGui::BeginGroup();
            static const char* stealLabels[] = {"QUALITY", "FAST", "SCAN"};
            int steal = static_cast<int>((std::min)(2u, w.stealPolicy));
            if (PanelKeys("STEAL POLICY", &steal, stealLabels, 3,
                          "Quality (recommended, and clearly best in listening tests) steals the quietest voice via an incremental priority index. Fast cursor and Scan avoid the index but audibly degrade victim quality under dense churn: kept for experimentation only.")) {
                w.stealPolicy = static_cast<uint32_t>(steal);
                doc.MarkDirty();
                send(svms::RLCommandType::SetStealPolicy, w.stealPolicy);
            }
            ImGui::EndGroup();

            ImGui::Dummy(ImVec2(0.0f, 8.0f));
            if (ImGui::BeginTable("##synth_knobs", 3, ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextRow();
                bool committed = false;

                ImGui::TableNextColumn();
                float retireDb = 20.0f * std::log10(w.voiceRetireThreshold);
                retireDb = (std::max)(-100.0f, (std::min)(-26.0f, retireDb));
                if (PanelKnob("RETIRE FLOOR", &retireDb, -100.0f, -26.0f, -76.0f,
                              "%.0f dB", 60.0f,
                              "Release-tail loudness where finished voices are freed. Higher (less negative) frees quiet tails sooner and cuts release cost on dense songs; lower keeps longer tails. Applies to newly started releases.",
                              &committed)) {
                    w.voiceRetireThreshold = std::pow(10.0f, retireDb / 20.0f);
                    doc.MarkDirty();
                }
                if (committed) {
                    uint32_t bits = 0u;
                    std::memcpy(&bits, &w.voiceRetireThreshold, sizeof(bits));
                    send(svms::RLCommandType::SetVoiceRetireFloor, bits);
                }

                ImGui::TableNextColumn();
                float cap = static_cast<float>(w.perKeyVoiceCap);
                if (PanelKnob("PER-KEY CAP", &cap, 0.0f, 32.0f, 0.0f,
                              cap < 0.5f ? "OFF" : "%.0f / key", 60.0f,
                              "Opt-in. Limits how many still-playing voices one (channel, note) may hold; a note-on at the cap replaces the oldest voice of that key. Off by default. Tames dense Black MIDI that hammers the same keys across channels.",
                              &committed)) {
                    w.perKeyVoiceCap = static_cast<uint32_t>(cap + 0.5f);
                    doc.MarkDirty();
                }
                if (committed) send(svms::RLCommandType::SetPerKeyVoiceCap, w.perKeyVoiceCap);

                ImGui::TableNextColumn();
                float ghosts = static_cast<float>(w.ghostBudget);
                if (PanelKnob("GHOST BUDGET", &ghosts, 0.0f, 4096.0f, 0.0f,
                              ghosts < 0.5f ? "OFF" : "%.0f", 60.0f,
                              "Optional cap on displaced-voice ghosts rendered per block (off = unbounded). Lower values cut render cost in extreme steal storms at the price of clicks on voices past the cap.",
                              &committed)) {
                    w.ghostBudget = static_cast<uint32_t>(ghosts + 0.5f);
                    doc.MarkDirty();
                }
                if (committed) send(svms::RLCommandType::SetGhostBudget, w.ghostBudget);
                ImGui::EndTable();
            }
        }
        EndRackPanel();

        // ------------------------------------------------------- ENGINE
        ImGui::TableNextColumn();
        if (BeginRackPanel("ENGINE")) {
            static const int threadValues[] = {0, 1, 2, 4, 8, 12, 16, 24, 32, 64};
            static const char* threadLabels[] = {"AUTO", "1", "2", "4", "8", "12", "16", "24", "32", "64"};
            int threadIdx = -1;
            for (int i = 0; i < 10; ++i)
                if (threadValues[i] == static_cast<int>(w.renderThreads)) threadIdx = i;
            if (PanelKeys("RENDER THREADS", &threadIdx, threadLabels, 10,
                          "Total voice-render lanes. 1 keeps voice rendering on the audio thread; AUTO lets V3 pick.",
                          true) && threadIdx >= 0) {
                w.renderThreads = static_cast<uint32_t>(threadValues[threadIdx]);
                doc.MarkDirty();
            }
            if (threadIdx < 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("custom: %u", w.renderThreads);
            }
            ImGui::Spacing();

            static const char* affinityLabels[] = {"OFF", "ALL ON P", "RT ON P"};
            int affinity = static_cast<int>((std::min)(2u, w.threadAffinityMode));
            if (PanelKeys("THREAD AFFINITY  (LIVE)", &affinity, affinityLabels, 3,
                          "Opt-in. ALL ON P pins every render thread to performance cores; RT ON P keeps audio and the compiler on P-cores and parks render workers on efficiency cores. No effect on non-hybrid CPUs.")) {
                w.threadAffinityMode = static_cast<uint32_t>(affinity);
                doc.MarkDirty();
                send(svms::RLCommandType::SetThreadAffinityMode, w.threadAffinityMode);
            }
            ImGui::Spacing();
            ImGui::Spacing();

            if (PanelLever("LARGE PAGES", &w.largePages,
                           "Optional. Backs the voice pool and dense-render storage with 2 MB pages to reduce TLB pressure at very large pools. Needs the Lock Pages In Memory privilege; otherwise silently falls back to normal allocation.",
                           true)) {
                doc.MarkDirty();
            }
            ImGui::Spacing();
            bool correctness = w.correctnessMode;
            if (PanelLever("FULL CORRECTNESS", &correctness,
                           "Renders the complete configured pool at full quality. Disable only if you understand the quality tradeoff.")) {
                w.correctnessMode = correctness;
                doc.MarkDirty();
                PushLiveBool(svms::RLCommandType::SetCorrectnessMode, correctness);
            }
            ImGui::Spacing();
            ImGui::Spacing();

            int memory = static_cast<int>(w.voiceMemoryBudgetMB);
            if (PanelLcdInt("VOICE MEMORY  (MIB)", &memory, 0, 65536,
                            "Upper bound for the voice pool, renderer scratch, dense-planner state and worker mix buffers. Zero means unlimited. A restart recalculates the largest safe live-growth ceiling.",
                            true, nullptr, "unlimited")) {
                w.voiceMemoryBudgetMB = static_cast<uint32_t>(memory);
                doc.MarkDirty();
            }
        }
        EndRackPanel();
        ImGui::EndTable();
    }

    if (w.maxVoices > 4096) {
        ImGui::PushStyleColor(ImGuiCol_Text, GetWarning());
        ImGui::TextWrapped("Extreme voice capacity. Actual realtime performance is workload and CPU dependent.");
        ImGui::PopStyleColor();
    }

    // ----------------------------------------------------- RENDER BUDGET
    if (BeginRackPanel("LIVE RENDER BUDGET")) {
        const ThemeSettings& th = GetThemeSettings();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool tel = lc.connected && t;
        struct Cell { const char* label; char value[48]; };
        Cell cells[8];
        auto set = [&](int i, const char* label, const char* fmt, auto... args) {
            cells[i].label = label;
            std::snprintf(cells[i].value, sizeof(cells[i].value), fmt, args...);
        };
        if (tel) {
            const float budgetMs = BlockBudgetMs(*t);
            const float load = (std::max)(0.0f, t->cpuLoadPercent);
            const uint32_t cap = t->live.maxVoices != 0u ? t->live.maxVoices : t->maxVoices;
            set(0, "ACTIVE VOICES", "%u / %u", t->activeVoices, cap);
            set(1, "RENDER / BUDGET", "%.2f / %.2f ms", PercentToMs(load, budgetMs), budgetMs);
            set(2, "RENDER LOAD", "%.1f%%", load);
            set(3, "HEADROOM", "%.1f%%", (std::max)(0.0f, 100.0f - load));
            set(4, "P95", "%.2f ms", PercentToMs(t->callbackP95Percent, budgetMs));
            set(5, "P99", "%.2f ms", PercentToMs(t->callbackP99Percent, budgetMs));
            set(6, "P99.9", "%.2f ms", PercentToMs(t->callbackP999Percent, budgetMs));
            set(7, "OVER BUDGET", "%llu  (streak %u)",
                static_cast<unsigned long long>(t->overBudgetCallbacks),
                t->maxConsecutiveOverBudget);
        } else {
            const char* labels[8] = {"ACTIVE VOICES", "RENDER / BUDGET", "RENDER LOAD", "HEADROOM",
                                     "P95", "P99", "P99.9", "OVER BUDGET"};
            for (int i = 0; i < 8; ++i) set(i, labels[i], "--");
        }
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        PushMono(0.95f);
        const float lh = ImGui::GetTextLineHeight();
        const float height = lh * 4.0f + 44.0f;
        DrawLcdFrame(dl, p0, ImVec2(p0.x + width, p0.y + height));
        for (int i = 0; i < 8; ++i) {
            const int col = i % 4;
            const int row = i / 4;
            const float x = p0.x + 16.0f + col * (width - 32.0f) / 4.0f;
            const float y = p0.y + 12.0f + row * (lh * 2.0f + 14.0f);
            dl->AddText(ImVec2(x, y),
                        ImGui::GetColorU32(ImVec4(th.accent.x, th.accent.y, th.accent.z, 0.5f)),
                        cells[i].label);
            dl->AddText(ImVec2(x, y + lh + 2.0f), ImGui::GetColorU32(th.accent), cells[i].value);
        }
        PopMono();
        ImGui::Dummy(ImVec2(width, height));
        if (tel) {
            ImGui::TextDisabled("Physical pool %u voices  |  buffer %u frames @ %u Hz  |  decimation %ux",
                                t->maxVoices, t->bufferFrames, t->sampleRate,
                                (std::max)(1u, t->decimationStep));
        } else {
            ImGui::TextDisabled("Connect to a running driver to see live render load.");
        }
    }
    EndRackPanel();
}

} // namespace svms::cfg
