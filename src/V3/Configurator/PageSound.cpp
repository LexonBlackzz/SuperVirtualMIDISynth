#include "PageSound.h"
#include "Anim.h"
#include "ConfigDocument.h"
#include "ConfiguratorApp.h"
#include "DriverLink.h"
#include "ModernWidgets.h"
#include "Platform.h"
#include "PlatformUtil.h"
#include "SVMSBuildInfo.h"
#include "Theme.h"
#include "Widgets.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "../SVMSRuntimeLinkProtocol.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

namespace svms::cfg {
namespace {

using C = svms::RLCommandType;

// --- SoundFont list model (moved from the old Audio page) ---------------------------

void SwapSoundFonts(ConfigValues& values, uint32_t a, uint32_t b) {
    if (a >= values.soundFontPaths.size() || b >= values.soundFontPaths.size()) return;
    std::swap(values.soundFontPaths[a], values.soundFontPaths[b]);
    for (SoundFontRouteValue& route : values.soundFontRoutes) {
        if (route.soundFontIndex == a) route.soundFontIndex = b;
        else if (route.soundFontIndex == b) route.soundFontIndex = a;
    }
    values.soundFontPath = values.soundFontPaths.empty() ? std::wstring() : values.soundFontPaths.front();
}

void RemoveSoundFont(ConfigValues& values, uint32_t index) {
    if (index >= values.soundFontPaths.size()) return;
    values.soundFontPaths.erase(values.soundFontPaths.begin() + index);
    for (auto it = values.soundFontRoutes.begin(); it != values.soundFontRoutes.end();) {
        if (it->soundFontIndex == index) {
            it = values.soundFontRoutes.erase(it);
        } else {
            if (it->soundFontIndex > index) --it->soundFontIndex;
            ++it;
        }
    }
    values.soundFontPath = values.soundFontPaths.empty() ? std::wstring() : values.soundFontPaths.front();
}

// True when the path was added (not a duplicate, room left in the 16-bank stack).
bool AddSoundFont(ConfigValues& values, const std::wstring& path) {
    if (values.soundFontPaths.size() >= 16u) return false;
    for (const std::wstring& existing : values.soundFontPaths)
        if (_wcsicmp(existing.c_str(), path.c_str()) == 0) return false;
    values.soundFontPaths.push_back(path);
    if (values.soundFontPath.empty()) values.soundFontPath = values.soundFontPaths.front();
    return true;
}

bool BrowseSoundFont(std::wstring& path, std::wstring& lastDirectory, const wchar_t* title) {
#ifndef _WIN32
    std::string chosen;
    if (!util::PickFile(false, util::WideToUtf8(title), util::WideToUtf8(lastDirectory), "*.sf2 *.sfz", chosen))
        return false;
    path = util::Utf8ToWide(chosen);
    lastDirectory = fs::path(path).parent_path().wstring();
    return true;
#else
    wchar_t fileBuf[1024] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"SoundFont files (*.sf2;*.sfz)\0*.sf2;*.sfz\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = static_cast<DWORD>(_countof(fileBuf));
    ofn.lpstrInitialDir = lastDirectory.empty() ? nullptr : lastDirectory.c_str();
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    path = fileBuf;
    lastDirectory = fs::path(path).parent_path().wstring();
    return true;
#endif
}

// --- Live load status (same RuntimeLink traffic as before) ---------------------------

struct SoundFontLiveStatus {
    uint32_t state = 0u;
    uint64_t requested = 0u;
    uint64_t activated = 0u;
    float pollTimer = 0.25f;
    std::string message = "Ready";
    bool error = false;
};

bool ParseU64Field(char*& cursor, uint64_t& value) {
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(cursor, &end, 10);
    if (errno != 0 || end == cursor || *end != '\t') return false;
    value = static_cast<uint64_t>(parsed);
    cursor = end + 1;
    return true;
}

void PollSoundFontStatus(svms::RuntimeLinkClient& client, SoundFontLiveStatus& status) {
    status.pollTimer += ImGui::GetIO().DeltaTime;
    if (status.pollTimer < 0.25f) return;
    status.pollTimer = 0.0f;
    char result[svms::kRuntimeLinkResultTextCapacity]{};
    if (client.SendCommand(C::QuerySoundFontLoad, 0u, 0u, svms::RuntimeLiveStateV2{}, 100u, result) !=
        svms::RLResult::Ok)
        return;
    char* cursor = result;
    uint64_t state = 0u;
    if (!ParseU64Field(cursor, state) || !ParseU64Field(cursor, status.requested)) return;
    char* end = nullptr;
    errno = 0;
    status.activated = std::strtoull(cursor, &end, 10);
    if (errno != 0 || end == cursor) return;
    cursor = *end == '\t' ? end + 1 : end;
    status.state = static_cast<uint32_t>(state);
    status.error = status.state == 4u;
    if (status.error) status.message = *cursor ? cursor : "SoundFont load failed";
    else if (status.state == 1u) status.message = "Loading and preparing SoundFont off-thread...";
    else if (status.state == 2u) status.message = "Prepared; waiting for the next audio block...";
    else if (status.state == 3u) status.message = "SoundFont activated";
    else status.message = "Ready";
}

void StartSoundFontLoad(svms::RuntimeLinkClient& client, const std::wstring& path, SoundFontLiveStatus& status) {
    const std::string utf8 = util::WideToUtf8(path);
    if (utf8.empty() || utf8.size() >= svms::kRuntimeLinkCommandTextCapacity) {
        status.message = "The SoundFont path is too long for RuntimeLink.";
        status.error = true;
        return;
    }
    char result[svms::kRuntimeLinkResultTextCapacity]{};
    const svms::RLResult code = client.SendCommand(C::ReloadSoundFont, 0u, 0u, svms::RuntimeLiveStateV2{}, 500u,
                                                   result, utf8.c_str());
    status.error = code != svms::RLResult::Ok;
    status.message = result[0] ? result : svms::RLV2_ResultToString(code);
    if (!status.error) {
        status.state = 1u;
        status.pollTimer = 0.25f;
    }
}

// --- Drawing helpers -----------------------------------------------------------------

constexpr float kPi = 3.14159265358979f;

void GripDots(ImDrawList* dl, ImVec2 p, float s, ImU32 col) {
    for (int cx = 0; cx < 2; ++cx)
        for (int cy = 0; cy < 3; ++cy)
            dl->AddCircleFilled(ImVec2(p.x + (2.5f + cx * 5.0f) * s, p.y + (3.0f + cy * 6.0f) * s), 1.2f * s, col, 8);
}

void SoundFontList(ConfigDocument& doc, bool liveSoundFont, SoundFontLiveStatus& status) {
    ConfigValues& w = doc.Working();
    const ui::Tokens& k = ui::Tok();
    const float s = ui::Scale();
    const LiveLinkContext& live = GetLiveLinkContext();
    static int g_drag = -1;

    ui::PushText(1, 14.0f);
    const float nameH = ImGui::GetTextLineHeight();
    ui::PopFont();
    ui::PushNum(12.0f);
    const float pathH = ImGui::GetTextLineHeight();
    ui::PopFont();
    const float rowH = 24.0f * s + nameH + 2.0f * s + pathH;
    const float gap = 8.0f * s;
    const int n = static_cast<int>(w.soundFontPaths.size());
    const float listTop = ImGui::GetCursorScreenPos().y;
    const bool busy = status.state == 1u || status.state == 2u;
    const bool canLoad = liveSoundFont && !w.soundFontPath.empty() && !busy;
    int removeIdx = -1;
    bool loadNow = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (int i = 0; i < n; ++i) {
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const bool dragging = g_drag == i;
        const float lift = AnimEase(ImGui::GetID("##lift"), dragging ? 1.0f : 0.0f, 0.2f);
        if (lift > 0.01f)
            ui::SoftShadow(dl, p, ImVec2(p.x + width, p.y + rowH), 10.0f * s, ui::WithAlpha(IM_COL32(0, 0, 0, 255), 0.5f * lift),
                           14.0f * s, 6);
        dl->AddRectFilled(p, ImVec2(p.x + width, p.y + rowH), k.card2, 10.0f * s);
        dl->AddRect(p, ImVec2(p.x + width, p.y + rowH), ui::MixU32(k.line2, k.acc, lift), 10.0f * s);

        ImGui::SetCursorScreenPos(ImVec2(p.x + 6.0f * s, p.y));
        ImGui::InvisibleButton("##grip", ImVec2(30.0f * s, rowH));
        if (ImGui::IsItemActivated()) g_drag = i;
        if (ImGui::IsItemHovered() || dragging) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        GripDots(dl, ImVec2(p.x + 16.0f * s, p.y + (rowH - 18.0f * s) * 0.5f), s,
                 ui::WithAlpha(k.muted, ImGui::IsItemHovered() || dragging ? 0.95f : 0.6f));

        const ImVec2 removeSz = ui::ButtonSize("Remove"), loadSz = ui::ButtonSize("Load now");
        const float bx = p.x + width - 16.0f * s - removeSz.x;
        const float by = p.y + (rowH - removeSz.y) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(bx, by));
        if (ui::Button("Remove")) removeIdx = i;
        float textRight = bx - 12.0f * s;
        if (i == 0) {
            ImGui::SetCursorScreenPos(ImVec2(bx - 12.0f * s - loadSz.x, by));
            if (ui::Button("Load now", ui::ButtonKind::Normal, ImVec2(0, 0), canLoad)) loadNow = true;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(liveSoundFont
                                           ? "Loads this SoundFont into the running engine. Loading silences active voices; MIDI state is kept."
                                           : "Needs a running SVMS to load live. Save to use it at the next start.");
                ImGui::EndTooltip();
            }
            textRight -= loadSz.x + 12.0f * s;
        }

        const fs::path sf(w.soundFontPaths[static_cast<size_t>(i)]);
        const std::string name = util::WideToUtf8(sf.filename().wstring());
        const std::string path = util::WideToUtf8(sf.wstring());
        const float tx = p.x + 16.0f * s + 10.0f * s + 12.0f * s;
        const float ty = p.y + (rowH - nameH - 2.0f * s - pathH) * 0.5f;
        dl->PushClipRect(ImVec2(tx, p.y), ImVec2(textRight, p.y + rowH), true);
        ui::PushText(1, 14.0f);
        dl->AddText(ImVec2(tx, ty), k.text, name.c_str());
        const float nameW = ImGui::CalcTextSize(name.c_str()).x;
        ui::PopFont();
        ui::PushNum(12.0f);
        dl->AddText(ImVec2(tx, ty + nameH + 2.0f * s), k.muted, path.c_str());
        ui::PopFont();
        if (i == 0) {
            ImGui::SetCursorScreenPos(ImVec2(tx + nameW + 8.0f * s, ty + (nameH - ui::ChipSize("Primary").y) * 0.5f));
            ui::Chip("Primary", ui::ChipKind::Acc);
        }
        dl->PopClipRect();

        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH + gap - ImGui::GetStyle().ItemSpacing.y));
        ImGui::Dummy(ImVec2(1.0f, 0.0f));
        ImGui::PopID();
    }

    // Drag to reorder: the row follows the pointer one slot at a time.
    if (g_drag >= 0) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || g_drag >= n) {
            g_drag = -1;
        } else {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            const int target = ImClamp(static_cast<int>(std::floor((ImGui::GetIO().MousePos.y - listTop) / (rowH + gap))), 0, n - 1);
            while (g_drag < target) { SwapSoundFonts(w, g_drag, g_drag + 1); ++g_drag; doc.MarkDirty(); }
            while (g_drag > target) { SwapSoundFonts(w, g_drag, g_drag - 1); --g_drag; doc.MarkDirty(); }
        }
    }
    if (removeIdx >= 0) {
        RemoveSoundFont(w, static_cast<uint32_t>(removeIdx));
        doc.MarkDirty();
    }
    if (loadNow && live.client) StartSoundFontLoad(*live.client, w.soundFontPath, status);

    // Drop zone.
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        ui::PushText(0, 13.0f);
        const float h = 28.0f * s + ImGui::GetTextLineHeight();
        const bool hov = ImGui::IsMouseHoveringRect(p, ImVec2(p.x + width, p.y + h));
        const float e = AnimEase(ImGui::GetID("##dropHov"), hov ? 1.0f : 0.0f, 0.2f);
        dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), ui::WithAlpha(k.acc, 0.16f * e), 10.0f * s);
        ui::DashedRect(dl, p, ImVec2(p.x + width, p.y + h), 10.0f * s, ui::MixU32(k.line2, k.acc, e));
        const char* text = "Drop .sf2 / .sfz files here, or drag rows to reorder";
        const ImVec2 ts = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2(p.x + (width - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), k.muted, text);
        ui::PopFont();
        ImGui::Dummy(ImVec2(width, h));
    }

    // Status line: live load progress, or the restart note when offline.
    ImGui::Dummy(ImVec2(0.0f, 6.0f * s - ImGui::GetStyle().ItemSpacing.y));
    if (liveSoundFont) {
        ui::PushNum(12.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, status.error ? k.bad : (busy ? k.acc : k.ok));
        ImGui::TextUnformatted(status.message.c_str());
        ImGui::PopStyleColor();
        if (live.telemetry && live.telemetry->soundFontName[0] != '\0') {
            ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
            ImGui::Text("Active: %s", live.telemetry->soundFontName);
            ImGui::PopStyleColor();
        }
        ui::PopFont();
    }
    if (n > 1 || !liveSoundFont) {
        ui::RestartMark(true);
        ImGui::SameLine(0.0f, 6.0f * s);
        ui::PushText(0, 11.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
        ImGui::TextUnformatted(liveSoundFont ? "for extra SoundFonts and their order."
                                              : "Connect to a running SVMS to switch the first SoundFont live.");
        ImGui::PopStyleColor();
        ui::PopFont();
    }
}

// --- Velocity graph --------------------------------------------------------------------

void VelocityGraph(const ConfigValues& w, float width) {
    const ui::Tokens& k = ui::Tok();
    const float s = ui::Scale();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = 150.0f * s;
    const ImVec2 q(p.x + width, p.y + h);
    dl->AddRectFilled(p, q, k.bg, 12.0f * s);
    dl->AddRect(p, q, k.line, 12.0f * s);
    dl->AddLine(ImVec2(p.x, p.y + h * 0.5f), ImVec2(q.x, p.y + h * 0.5f), k.line);
    dl->AddLine(ImVec2(p.x + width * 0.5f, p.y), ImVec2(p.x + width * 0.5f, q.y), k.line);

    // 120 segments, drawn straight from the values (no animation while dragging).
    constexpr int kPoints = 120;
    std::vector<ImVec2> pts;
    pts.reserve(kPoints + 1);
    for (int i = 0; i <= kPoints; ++i) {
        const float x = static_cast<float>(i) / kPoints;
        const float vel = x * 127.0f;
        const float y = vel < static_cast<float>(w.velocityIgnoreBelow)
                            ? 0.0f
                            : w.velocityFloor + (1.0f - w.velocityFloor) * std::pow(x, w.velocityCurve);
        pts.push_back(ImVec2(p.x + x * width, p.y + (140.0f - y * 126.0f) / 150.0f * h));
    }
    dl->PushClipRect(ImVec2(p.x + 1.0f, p.y + 1.0f), ImVec2(q.x - 1.0f, q.y - 1.0f), true);
    const ImDrawListFlags oldFlags = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;   // no seams between the strips
    for (int i = 0; i < kPoints; ++i) {
        const ImVec2 quad[4] = {pts[i], pts[i + 1], ImVec2(pts[i + 1].x, q.y), ImVec2(pts[i].x, q.y)};
        dl->AddConvexPolyFilled(quad, 4, k.accSoft);
    }
    dl->Flags = oldFlags;
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), k.acc, ImDrawFlags_RoundCornersAll, 2.5f * s);
    dl->PopClipRect();

    ui::PushNum(11.0f);
    dl->AddText(ImVec2(p.x + 12.0f * s, p.y + 10.0f * s), k.acc, "Loudness");
    const float lh = ImGui::GetTextLineHeight();
    const char* axis[3] = {"Soft", "Velocity", "Hard"};
    const float ay = q.y + 6.0f * s;
    dl->AddText(ImVec2(p.x + 2.0f * s, ay), k.muted, axis[0]);
    dl->AddText(ImVec2(p.x + (width - ImGui::CalcTextSize(axis[1]).x) * 0.5f, ay), k.muted, axis[1]);
    dl->AddText(ImVec2(q.x - 2.0f * s - ImGui::CalcTextSize(axis[2]).x, ay), k.muted, axis[2]);
    ui::PopFont();
    ImGui::Dummy(ImVec2(width, h + 6.0f * s + lh));
}

float KnobBlockHeight() { return ui::KnobSize().y; }

void VelocityCard(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const ui::Tokens& k = ui::Tok();
    const float s = ui::Scale();
    ui::BeginCard("##velocity", "Velocity", nullptr, ui::RestartMarkWidth(), "velCurve");
    ui::PushText(0, 11.0f);
    const float markH = ImGui::GetTextLineHeight();
    ui::PopFont();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (38.0f * s - markH) * 0.5f);   // centre on the title
    ui::RestartMark();
    ui::CardHeaderEnd();

    const float total = ImGui::GetContentRegionAvail().x;
    const float gap = 16.0f * s;
    const float leftW = 324.0f * s;   // three 96px knobs + gaps
    const float colW = (std::max)(120.0f * s, total - gap - leftW);   // graph takes the rest
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const char* hint = "Drag up or down. Hold Shift for fine steps. Double-click to reset.";
    ui::PushText(0, 12.0f);
    const float hintH = ImGui::CalcTextSize(hint, nullptr, false, leftW).y;
    ui::PopFont();
    const float leftH = KnobBlockHeight() + 10.0f * s + hintH;
    const float rightH = 150.0f * s + 6.0f * s + 14.0f * s;
    const float rowH = (std::max)(leftH, rightH);

    // Right column: the response graph.
    ImGui::SetCursorScreenPos(ImVec2(origin.x + leftW + gap, origin.y + (rowH - rightH) * 0.5f));
    VelocityGraph(w, colW);

    // Left column: three knobs.
    const float y = origin.y + (rowH - leftH) * 0.5f;
    bool committed = false;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
    const ui::KnobSpec curve = {"Curve", 0.1f, 10.0f, 1.0f, 0.01f, true, "%.2f"};
    if (ui::Knob("##vcurve", &w.velocityCurve, curve, &committed)) doc.MarkDirty();
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 114.0f * s, y));
    const ui::KnobSpec floorK = {"Floor", 0.0f, 0.99f, 0.0f, 0.01f, false, "%.2f"};
    if (ui::Knob("##vfloor", &w.velocityFloor, floorK, &committed)) doc.MarkDirty();
    ui::Anchor("velFloor", ImVec2(origin.x + 114.0f * s, y), ImVec2(origin.x + (114.0f + 96.0f) * s, y + KnobBlockHeight()));
    float ignore = static_cast<float>(w.velocityIgnoreBelow);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 228.0f * s, y));
    const ui::KnobSpec ignoreK = {"Ignore below", 0.0f, 127.0f, 0.0f, 1.0f, false, "%.0f", nullptr, "Off"};
    if (ui::Knob("##vignore", &ignore, ignoreK, &committed)) {
        w.velocityIgnoreBelow = static_cast<uint32_t>(ignore + 0.5f);
        doc.MarkDirty();
    }
    ui::Anchor("velIgnore", ImVec2(origin.x + 228.0f * s, y), ImVec2(origin.x + (228.0f + 96.0f) * s, y + KnobBlockHeight()));
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y + KnobBlockHeight() + 10.0f * s));
    ui::PushText(0, 12.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + leftW);
    ImGui::TextUnformatted(hint);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rowH - ImGui::GetStyle().ItemSpacing.y));
    ImGui::Dummy(ImVec2(total, 0.0f));
    ui::EndCard();
}

// --- Voice character -----------------------------------------------------------------

void VoiceCharacterCard(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const ui::Tokens& k = ui::Tok();
    const float s = ui::Scale();
    static const char* labels[5] = {"Off", "Analytic", "Sweep", "Diffuse", "Random"};
    static const char* descs[5] = {
        "Every voice starts in phase. The exact reference sound, and the cheapest.",
        "Each voice gets its own fixed random phase. The Black MIDI hum stops stacking up; loudness, tone and timing stay the same.",
        "Like Analytic, but the phase slowly turns (0.25 Hz), adding gentle movement.",
        "Smears each voice through an allpass filter. Softer attacks, on purpose.",
        "Like Sweep, but every voice turns at its own speed and direction."};
    const char* chip = "About 2\xC3\x97 CPU when on";
    ui::BeginCard("##phase", "Voice character", nullptr, ui::ChipSize(chip).x, "phase");
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (38.0f * s - ui::ChipSize(chip).y) * 0.5f);
    ui::Chip(chip, ui::ChipKind::Cpu);
    ui::CardHeaderEnd();
    int phase = static_cast<int>((std::min)(4u, w.phaseRotationMode));
    if (ui::Segmented("##phaseseg", &phase, labels, 5, -1.0f)) {
        w.phaseRotationMode = static_cast<uint32_t>(phase);
        doc.MarkDirty();
        SendLiveCommand(C::SetPhaseRotation, w.phaseRotationMode);
    }
    ImGui::Dummy(ImVec2(0.0f, 12.0f * s - ImGui::GetStyle().ItemSpacing.y));

    // Info box: bold mode name, then its description, wrapped as one paragraph.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float px = 18.0f * s, py = 14.0f * s;
    std::string name = std::string(labels[phase]) + ".";
    ui::PushText(1, 13.0f);
    const float nameW = ImGui::CalcTextSize(name.c_str()).x;
    ui::PopFont();
    ui::PushText(0, 13.0f);
    const float spaceW = ImGui::CalcTextSize(" ").x;
    const std::string lead(static_cast<size_t>(std::ceil((nameW + 4.0f * s) / spaceW)), ' ');
    const std::string text = lead + descs[phase];
    const float wrapW = width - 2.0f * px;
    const ImVec2 ts = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrapW);
    const float h = ts.y + 2.0f * py;
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), k.card2, 10.0f * s);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + px, p.y + py), k.muted, text.c_str(), nullptr, wrapW);
    ui::PopFont();
    ui::PushText(1, 13.0f);
    dl->AddText(ImVec2(p.x + px, p.y + py), k.text, name.c_str());
    ui::PopFont();
    ImGui::Dummy(ImVec2(width, h));
    ui::EndCard();
}

// --- Routes ----------------------------------------------------------------------------

void RoutesCard(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const ui::Tokens& k = ui::Tok();
    const float s = ui::Scale();
    ui::BeginReveal("routes");
    ui::BeginCard("##routes");
    const bool canAdd = !w.soundFontPaths.empty() && w.soundFontRoutes.size() < 256u;
    const bool none = w.soundFontRoutes.empty();
    char title[48];
    std::snprintf(title, sizeof(title), "%u route%s", static_cast<unsigned>(w.soundFontRoutes.size()),
                  w.soundFontRoutes.size() == 1 ? "" : "s");
    ui::BeginRow("routes", none ? "No routes yet" : title,
                 "Without routes, the first SoundFont in the list that has the instrument plays it.",
                 ui::ButtonSize("Add route"));
    if (ui::Button("Add route", ui::ButtonKind::Normal, ImVec2(0, 0), canAdd)) {
        SoundFontRouteValue route{};
        route.soundFontIndex = static_cast<uint32_t>(w.soundFontPaths.size() > 1u ? 1u : 0u);
        w.soundFontRoutes.push_back(route);
        doc.MarkDirty();
    }
    ui::EndRow();

    int removeIdx = -1;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (uint32_t i = 0u; i < w.soundFontRoutes.size(); ++i) {
        SoundFontRouteValue& route = w.soundFontRoutes[i];
        ImGui::PushID(static_cast<int>(0x4000u + i));
        const float pad = 14.0f * s;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        ImGui::SetCursorScreenPos(ImVec2(p.x + pad, p.y + pad));
        ImGui::BeginGroup();

        std::vector<std::string> names;
        std::vector<const char*> items;
        for (const std::wstring& path : w.soundFontPaths)
            names.push_back(util::WideToUtf8(fs::path(path).filename().wstring()));
        for (const std::string& n : names) items.push_back(n.c_str());
        int sf = static_cast<int>(route.soundFontIndex);
        if (ui::Select("##sf", &sf, items.data(), static_cast<int>(items.size()), 240.0f * s) && sf >= 0) {
            route.soundFontIndex = static_cast<uint32_t>(sf);
            doc.MarkDirty();
        }
        ImGui::SameLine(0.0f, 16.0f * s);
        {
            const float y0 = ImGui::GetCursorPosY();
            ImGui::SetCursorPosY(y0 + 5.0f * s);
            if (ui::Toggle("##drums", &route.percussion)) doc.MarkDirty();
            ImGui::SameLine(0.0f, 10.0f * s);
            ui::PushText(0, 13.0f);
            const ImVec2 tp = ImGui::GetCursorScreenPos();
            const float th = ImGui::GetTextLineHeight();
            dl->AddText(ImVec2(tp.x, tp.y + (28.0f * s - th) * 0.5f), k.text, "Drums");
            ImGui::Dummy(ImVec2(ImGui::CalcTextSize("Drums").x, 28.0f * s));
            ui::PopFont();
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, ImGui::GetCursorScreenPos().y));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
        ImGui::Dummy(ImVec2(0.0f, 8.0f * s));

        struct Field { const char* label; int value; int lo, hi; };
        Field fields[4] = {{"MIDI bank", static_cast<int>(route.targetBank), 0, 127},
                           {"MIDI preset", route.targetPreset, -1, 127},
                           {"Source bank", static_cast<int>(route.sourceBank), 0, 65535},
                           {"Source preset", route.sourcePreset, -1, 127}};
        const float fieldY = ImGui::GetCursorScreenPos().y;
        for (int f = 0; f < 4; ++f) {
            const float fx = p.x + pad + f * 126.0f * s;
            ImGui::SetCursorScreenPos(ImVec2(fx, fieldY));
            ui::PushText(0, 12.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
            ImGui::TextUnformatted(fields[f].label);
            ImGui::PopStyleColor();
            ui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(fx, ImGui::GetCursorScreenPos().y));
            ImGui::PushID(f);
            if (ui::NumberField("##v", &fields[f].value, fields[f].lo, fields[f].hi, 110.0f)) {
                doc.MarkDirty();
                if (f == 0) route.targetBank = static_cast<uint32_t>(fields[f].value);
                else if (f == 1) route.targetPreset = fields[f].value;
                else if (f == 2) route.sourceBank = static_cast<uint32_t>(fields[f].value);
                else route.sourcePreset = fields[f].value;
            }
            ImGui::PopID();
        }
        const ImVec2 rb = ui::ButtonSize("Remove");
        ImGui::SetCursorScreenPos(ImVec2(p.x + width - pad - rb.x, ImGui::GetItemRectMax().y - rb.y));
        if (ui::Button("Remove")) removeIdx = static_cast<int>(i);
        ImGui::EndGroup();
        const float bottom = ImGui::GetItemRectMax().y + pad;
        dl->ChannelsSetCurrent(0);
        dl->AddRectFilled(p, ImVec2(p.x + width, bottom), k.card2, 10.0f * s);
        dl->AddRect(p, ImVec2(p.x + width, bottom), k.line2, 10.0f * s);
        dl->ChannelsMerge();
        ImGui::SetCursorScreenPos(ImVec2(p.x, bottom + 8.0f * s - ImGui::GetStyle().ItemSpacing.y));
        ImGui::Dummy(ImVec2(1.0f, 0.0f));
        ImGui::PopID();
        if (removeIdx >= 0) break;
    }
    if (removeIdx >= 0) {
        w.soundFontRoutes.erase(w.soundFontRoutes.begin() + removeIdx);
        doc.MarkDirty();
    }
    ui::EndCard();
    ui::EndReveal();
}

} // namespace

int HandleDroppedSoundFonts(ConfigDocument& doc) {
    int added = 0;
    for (const std::string& utf8 : TakeDroppedFiles()) {
        const std::wstring path = util::Utf8ToWide(utf8);
        const std::wstring ext = fs::path(path).extension().wstring();
        if (_wcsicmp(ext.c_str(), L".sf2") != 0 && _wcsicmp(ext.c_str(), L".sfz") != 0) continue;
        if (AddSoundFont(doc.Working(), path)) ++added;
    }
    if (added) doc.MarkDirty();
    return added;
}

void DrawSoundPage(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const LiveLinkContext& live = GetLiveLinkContext();
    const bool liveSoundFont = live.connected && live.client &&
                               live.client->HasCapability(svms::build::CapabilitySoundFontReload);
    static SoundFontLiveStatus status;
    static std::wstring lastDir;
    if (liveSoundFont) PollSoundFontStatus(*live.client, status);
    if (lastDir.empty() && !w.soundFontPath.empty()) lastDir = fs::path(w.soundFontPath).parent_path().wstring();

    ui::BeginCard("##soundfonts", "SoundFonts", "The first one that has an instrument wins.",
                  ui::ButtonSize("+  Add SoundFont").x, "sf");
    if (ui::Button("+  Add SoundFont", ui::ButtonKind::Primary)) {
        std::wstring selected;
        if (BrowseSoundFont(selected, lastDir, L"Add SoundFont") && AddSoundFont(w, selected)) doc.MarkDirty();
    }
    ui::CardHeaderEnd();
    SoundFontList(doc, liveSoundFont, status);
    ui::EndCard();

    VelocityCard(doc);
    VoiceCharacterCard(doc);

    ui::BeginCard("##tuning", nullptr, nullptr, 0.0f, "tuning");
    {
        static const char* labels[2] = {"12-EDO", "31-EDO"};
        int tuning = w.tuningEdo == 31u ? 1 : 0;
        ui::BeginRow("tuning", "Tuning",
                     tuning ? "31 steps per octave; key 155 is middle C. Drums keep their normal mapping."
                            : "Normal 12 notes per octave.",
                     ui::SegmentedSize(labels, 2));
        if (ui::Segmented("##tuningseg", &tuning, labels, 2)) {
            w.tuningEdo = tuning ? 31u : 12u;
            doc.MarkDirty();
        }
        ui::EndRow();
    }
    ui::EndCard();

    if (ui::Disclosure("routes", "Bank and preset routes", "Send a bank/preset to a specific SoundFont"))
        RoutesCard(doc);
}

} // namespace svms::cfg
