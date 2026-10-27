#include "configurator/PagePerformance.h"
#include "configurator/Anim.h"
#include "configurator/ConfigDocument.h"
#include "configurator/ConfiguratorApp.h"
#include "configurator/DriverLink.h"
#include "configurator/ModernWidgets.h"
#include "configurator/Theme.h"
#include "configurator/Widgets.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace svms::cfg {

// SetDecimation param: mode in the low byte (0 off, 1 velocity, 2 level),
// muted-tail cull blocks in bits 8..15.
uint32_t DecimationParam(const ConfigValues& w) {
    const uint32_t mode = w.decimation ? (w.decimationByLevel ? 2u : 1u) : 0u;
    return mode | ((w.decimationCullBlocks & 0xFFu) << 8);
}

namespace {
struct ProfileDef {
    bool decim, fastTail, stealReject, coalesce, reexcite, ccCol, bendCol, omni, unbounded;
};
// Accurate: everything off. Balanced: only CC collapse. Extreme: decimation
// (by level), fast tail math, steal reject, coalescing, CC and bend collapse;
// block timing, never-skip-ahead and re-excite stay out.
const ProfileDef kProfiles[3] = {
    {false, false, false, false, false, false, false, false, false},
    {false, false, false, false, false, true, false, false, false},
    {true, true, true, true, false, true, true, false, false},
};
} // namespace

int MatchPerformanceProfile(const ConfigValues& w) {
    for (int i = 0; i < 3; ++i) {
        const ProfileDef& p = kProfiles[i];
        const bool coalesce = w.noteOnCollapseThreshold > 1u;
        if (w.decimation == p.decim && (!p.decim || w.decimationByLevel) &&
            w.fastChunkMath == p.fastTail && w.stealReject == p.stealReject &&
            coalesce == p.coalesce && w.noteOnReexcite == p.reexcite &&
            w.ccCollapse == p.ccCol && w.bendCollapse == p.bendCol &&
            w.blockTiming == p.omni && w.unboundedRender == p.unbounded)
            return i;
    }
    return -1;
}

void ApplyPerformanceProfile(ConfigDocument& doc, int profile) {
    if (profile < 0 || profile > 2) return;
    ConfigValues& w = doc.Working();
    const ConfigValues before = w;
    const ProfileDef& p = kProfiles[profile];
    w.decimation = p.decim;
    w.decimationByLevel = true;
    w.fastChunkMath = p.fastTail;
    w.stealReject = p.stealReject;
    w.noteOnCollapseThreshold = p.coalesce ? (before.noteOnCollapseThreshold > 1u ? before.noteOnCollapseThreshold : 32u) : 1u;
    w.noteOnReexcite = p.reexcite;
    w.ccCollapse = p.ccCol;
    w.bendCollapse = p.bendCol;
    w.blockTiming = p.omni;
    w.unboundedRender = p.unbounded;
    doc.MarkDirty();
    if (w.decimation != before.decimation || w.decimationByLevel != before.decimationByLevel)
        SendLiveCommand(SVMS_COMMAND_SET_DECIMATION, DecimationParam(w));
    if (w.fastChunkMath != before.fastChunkMath) SendLiveCommand(SVMS_COMMAND_SET_FAST_CHUNK_MATH, w.fastChunkMath ? 1u : 0u);
    if (w.stealReject != before.stealReject) SendLiveCommand(SVMS_COMMAND_SET_STEAL_REJECT, w.stealReject ? 1u : 0u);
    if (w.noteOnCollapseThreshold != before.noteOnCollapseThreshold)
        SendLiveCommand(SVMS_COMMAND_SET_NOTE_ON_COLLAPSE, w.noteOnCollapseThreshold);
    if (w.noteOnReexcite != before.noteOnReexcite) SendLiveCommand(SVMS_COMMAND_SET_NOTE_ON_REEXCITE, w.noteOnReexcite ? 1u : 0u);
    if (w.ccCollapse != before.ccCollapse) SendLiveCommand(SVMS_COMMAND_SET_CC_COLLAPSE, w.ccCollapse ? 1u : 0u);
    if (w.bendCollapse != before.bendCollapse) SendLiveCommand(SVMS_COMMAND_SET_BEND_COLLAPSE, w.bendCollapse ? 1u : 0u);
    if (w.blockTiming != before.blockTiming) SendLiveCommand(SVMS_COMMAND_SET_BLOCK_TIMING, w.blockTiming ? 1u : 0u);
    if (w.unboundedRender != before.unboundedRender) SendLiveCommand(SVMS_COMMAND_SET_UNBOUNDED_RENDER, w.unboundedRender ? 1u : 0u);
}

namespace {


constexpr float kDotW = 18.0f, kDotH = 5.0f;

std::string WithCommas(uint32_t v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return s;
}

void Wrapped(const char* text, ImU32 col, float px) {
    ui::PushText(0, px);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ui::PopFont();
}

// --- Profile cards -----------------------------------------------------------------

void ProfileCards(ConfigDocument& doc) {
    const ui::Tokens& k = ui::Tok();
    const float s = ui::Scale();
    static const char* names[3] = {"Accurate", "Balanced", "Extreme"};
    static const char* descs[3] = {
        "Every note exactly as written. For normal MIDI and for recording.",
        "Only the shortcuts that sound the same. A safe pick for dense files.",
        "Every shortcut on. For million-note Black MIDI in realtime."};
    const int match = MatchPerformanceProfile(doc.Working());
    const float gap = 12.0f * s;
    const float width = ImGui::GetContentRegionAvail().x;
    const float cardW = (width - 2.0f * gap) / 3.0f;
    const float pad = 20.0f * s;
    ui::PushText(2, 15.0f);
    const float nameH = ImGui::GetTextLineHeight();
    ui::PopFont();
    ui::PushText(0, 13.0f);
    float descH = 0.0f;
    for (int i = 0; i < 3; ++i)
        descH = (std::max)(descH, ImGui::CalcTextSize(descs[i], nullptr, false, cardW - 2.0f * pad).y);
    ui::PopFont();
    const float height = pad + nameH + 6.0f * s + descH + 6.0f * s + 4.0f * s + kDotH * s + pad;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    for (int i = 0; i < 3; ++i) {
        ImGui::PushID(i);
        const ImVec2 base(origin.x + i * (cardW + gap), origin.y);
        ImGui::SetCursorScreenPos(base);
        const bool clicked = ImGui::InvisibleButton("##pc", ImVec2(cardW, height));
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const bool hov = ImGui::IsItemHovered(), down = ImGui::IsItemActive();
        const bool sel = match == i;
        const float lift = AnimSpring(ImGui::GetID("##lift"), hov ? -3.0f * s : 0.0f);
        const float press = AnimSpring(ImGui::GetID("##press"), down ? 0.97f : 1.0f);
        const float lit = AnimEase(ImGui::GetID("##sel"), sel ? 1.0f : 0.0f, 0.3f);
        const ImVec2 c(base.x + cardW * 0.5f, base.y + height * 0.5f + lift);
        const ImVec2 a(c.x - cardW * 0.5f * press, c.y - height * 0.5f * press);
        const ImVec2 b(c.x + cardW * 0.5f * press, c.y + height * 0.5f * press);
        const float r = 12.0f * s;
        if (lit > 0.01f)
            ui::SoftShadow(dl, ImVec2(a.x, a.y + 8.0f * s), ImVec2(b.x, b.y + 8.0f * s), r, ui::WithAlpha(k.acc, 0.35f * lit), 18.0f * s, 6);
        dl->AddRectFilled(a, b, ui::MixU32(k.card2, ui::MixU32(k.card2, k.acc, 0.16f), lit), r);
        dl->AddRect(a, b, ui::MixU32(k.line2, k.acc, lit), r);
        float y = a.y + pad * press;
        const float x = a.x + pad * press;
        ui::PushText(2, 15.0f * press);
        dl->AddText(ImVec2(x, y), k.text, ui::Caps(names[i]));
        ui::PopFont();
        y += (nameH + 6.0f * s) * press;
        ui::PushText(0, 13.0f * press);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, y), k.muted, descs[i], nullptr,
                    (cardW - 2.0f * pad) * press);
        ui::PopFont();
        const float dotsY = b.y - (pad + kDotH * s) * press;
        for (int d = 0; d < 3; ++d) {
            const float dx = x + d * (kDotW + 4.0f) * s * press;
            dl->AddRectFilled(ImVec2(dx, dotsY), ImVec2(dx + kDotW * s * press, dotsY + kDotH * s * press),
                              d <= i ? k.acc : k.card3, 3.0f * s);
        }
        if (clicked) ApplyPerformanceProfile(doc, i);
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + height));
    ImGui::Dummy(ImVec2(width, 0.0f));
}

// --- Nested rows inside a tile -------------------------------------------------------

struct Nested {
    ImVec2 origin;
    float x1, leftBottom, ctlY;
    ImVec2 ctl;
    bool stacked;
};

Nested NestedBegin(const char* title, const char* hint, ImVec2 ctl, bool first) {
    const ui::Tokens& k = ui::Tok();
    const float s = ui::Scale();
    if (!first) ImGui::Dummy(ImVec2(0.0f, 14.0f * s - ImGui::GetStyle().ItemSpacing.y));
    Nested n{};
    n.origin = ImGui::GetCursorScreenPos();
    n.x1 = n.origin.x + ImGui::GetContentRegionAvail().x;
    ctl.x = (std::min)(ctl.x, n.x1 - n.origin.x);   // never wider than the card
    n.ctl = ctl;
    // Too narrow to sit beside the text: the control drops under it instead.
    n.stacked = n.x1 - n.origin.x - ctl.x < 80.0f * s + 14.0f * s;
    const float leftW = n.stacked ? n.x1 - n.origin.x
                                  : (std::max)(80.0f * s, n.x1 - n.origin.x - ctl.x - 14.0f * s);
    ImGui::BeginGroup();
    ui::PushText(0, 13.0f);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + leftW);
    ImGui::TextUnformatted(title);
    ImGui::PopTextWrapPos();
    ui::PopFont();
    if (hint && *hint) {
        ui::PushText(0, 12.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, k.muted);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + leftW);
        ImGui::TextUnformatted(hint);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ui::PopFont();
    }
    ImGui::EndGroup();
    n.leftBottom = ImGui::GetItemRectMax().y;
    const float leftH = n.leftBottom - n.origin.y;
    n.ctlY = n.stacked ? n.leftBottom + 6.0f * s : n.origin.y + (std::max)(0.0f, (leftH - ctl.y) * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(n.x1 - ctl.x, n.ctlY));
    return n;
}

void NestedEnd(const Nested& n) {
    const float bottom = (std::max)(n.leftBottom, n.ctlY + n.ctl.y);
    ImGui::SetCursorScreenPos(ImVec2(n.origin.x, bottom - ImGui::GetStyle().ItemSpacing.y));
    ImGui::Dummy(ImVec2(1.0f, 0.0f));
}

// Mono readout, right-aligned in `width`.
void ReadoutRight(const char* text, float width, ImU32 col) {
    ui::PushNum(13.0f);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + width - ts.x, p.y + (ui::Scale() * 28.0f - ts.y) * 0.5f), col, text);
    ui::PopFont();
    ImGui::Dummy(ImVec2(width, ui::Scale() * 28.0f));
}

// --- Shortcuts ---------------------------------------------------------------------

void GroupHeader(const char* title, const char* hint, bool first) {
    const ui::Tokens& k = ui::Tok();
    const float s = ui::Scale();
    ImGui::Dummy(ImVec2(0.0f, (first ? 4.0f : 22.0f) * s - ImGui::GetStyle().ItemSpacing.y));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ui::PushText(2, 13.0f);
    const float tw = ImGui::CalcTextSize(ui::Caps(title)).x;
    const float th = ImGui::GetTextLineHeight();
    dl->AddText(p, k.text, ui::Caps(title));
    ui::PopFont();
    ui::PushText(0, 12.0f);
    dl->AddText(ImVec2(p.x + tw + 12.0f * s, p.y + (th - ImGui::GetTextLineHeight()) * 0.5f), k.muted, hint);
    ui::PopFont();
    ImGui::Dummy(ImVec2(1.0f, th));
    ImGui::Dummy(ImVec2(0.0f, 12.0f * s - ImGui::GetStyle().ItemSpacing.y));
}

using ui::ChipDef;
using ui::ChipKind;

// One switch tile whose toggle just flips a bool and pushes a live command.
bool SwitchTile(ConfigDocument& doc, const char* anchor, const char* title, const char* desc,
                bool* value, uint32_t command, const ChipDef* tags, int tagCount, float delay) {
    bool changed = false;
    const bool on = ui::BeginTile(anchor, title, desc, value, &changed, tags, tagCount, delay);
    if (changed) {
        doc.MarkDirty();
        SendLiveCommand(command, *value ? 1u : 0u);
    }
    return on;
}

void Shortcuts(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const float s = ui::Scale();
    ui::BeginCard("##shortcuts", "Shortcuts",
                  "Ways to save CPU, grouped by what they cost. The profile above sets these for you.", 0.0f, "tradeoffs");

    // -------------------------------------------------------------- Free
    GroupHeader("Free", "No audible difference in practice. Safe to leave on.", true);
    ui::BeginTileGrid("##g_free");
    {
        const ChipDef tags[] = {{"Saves voices", ChipKind::Cpu}};
        SwitchTile(doc, "noteMerge", "Merge identical notes",
                   "The same key, velocity and exact time on several tracks becomes one voice at the combined volume. Nothing is dropped or moved.",
                   &w.noteMerge, SVMS_COMMAND_SET_NOTE_MERGE, tags, 1, 0.0f);
        ui::EndTile();
    }
    {
        const ChipDef tags[] = {{"Saves CPU", ChipKind::Cpu}};
        SwitchTile(doc, "ccCol", "Skip overwritten controller changes",
                   "Drops volume, pan and similar changes that a later one on the same channel replaces within a few milliseconds.",
                   &w.ccCollapse, SVMS_COMMAND_SET_CC_COLLAPSE, tags, 1, 0.04f);
        ui::EndTile();
    }
    ui::EndTileGrid();

    // ------------------------------------------------- Costs a little sound
    GroupHeader("Costs a little sound", "Big savings on dense Black MIDI. Quiet details can thin out.", false);
    ui::BeginTileGrid("##g_sound");
    {
        const ChipDef tags[] = {{"Saves a lot of CPU", ChipKind::Cpu}, {"Thins quiet layers", ChipKind::Bad}};
        bool changed = false;
        const bool on = ui::BeginTile("decim", "Voice decimation",
                                      "Above 10k / 25k / 50k / 150k voices, only the loudest half / quarter / eighth / sixteenth are mixed. The rest keep running silently.",
                                      &w.decimation, &changed, tags, 2, 0.08f);
        if (changed) {
            doc.MarkDirty();
            SendLiveCommand(SVMS_COMMAND_SET_DECIMATION, DecimationParam(w));
        }
        if (on) {
            ui::TileNested();
            Nested n = NestedBegin("Mute by actual loudness", "Fading tails go first, not soft new notes.",
                                   ImVec2(46.0f * s, 28.0f * s), true);
            if (ui::Toggle("##bylevel", &w.decimationByLevel, 0.12f)) {
                doc.MarkDirty();
                SendLiveCommand(SVMS_COMMAND_SET_DECIMATION, DecimationParam(w));
            }
            NestedEnd(n);
            ImGui::Dummy(ImVec2(0.0f, 14.0f * s - ImGui::GetStyle().ItemSpacing.y));
            const float avail = ImGui::GetContentRegionAvail().x;
            ui::PushText(0, 13.0f);
            const float lw = ImGui::CalcTextSize("Free muted tails after").x;
            const float lh = ImGui::GetTextLineHeight();
            ui::PopFont();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float rw = 72.0f * s, g = 14.0f * s;
            ui::PushText(0, 13.0f);
            ImGui::GetWindowDrawList()->AddText(ImVec2(p.x, p.y + (28.0f * s - lh) * 0.5f), ui::Tok().text,
                                                "Free muted tails after");
            ui::PopFont();
            ImGui::SetCursorScreenPos(ImVec2(p.x + lw + g, p.y + 3.0f * s));
            float cull = static_cast<float>(w.decimationCullBlocks);
            bool committed = false;
            if (ui::RangeSlider("##cull", &cull, 0.0f, 120.0f, (std::max)(60.0f * s, avail - lw - rw - 2.0f * g), &committed)) {
                const uint32_t v = static_cast<uint32_t>(cull + 0.5f);
                if (v != w.decimationCullBlocks) {
                    w.decimationCullBlocks = v;
                    doc.MarkDirty();
                }
            }
            if (committed && w.decimation) SendLiveCommand(SVMS_COMMAND_SET_DECIMATION, DecimationParam(w));
            char txt[32];
            if (w.decimationCullBlocks) std::snprintf(txt, sizeof(txt), "%u blocks", w.decimationCullBlocks);
            else std::snprintf(txt, sizeof(txt), "Never");
            ImGui::SetCursorScreenPos(ImVec2(p.x + avail - rw, p.y));
            ReadoutRight(txt, rw, ui::Tok().text);
        }
        ui::EndTile();
    }
    {
        const ChipDef tags[] = {{"Saves CPU", ChipKind::Cpu}, {"Drops quiet notes", ChipKind::Bad}};
        SwitchTile(doc, "stealReject", "Skip notes quieter than everything playing",
                   "When the voice limit is full, a note that would be the first one replaced is not played at all, like BASSMIDI.",
                   &w.stealReject, SVMS_COMMAND_SET_STEAL_REJECT, tags, 2, 0.16f);
        ui::EndTile();
    }
    {
        const ChipDef tags[] = {{"Saves CPU", ChipKind::Cpu}, {"Loses retrigger rhythm", ChipKind::Bad}};
        bool on = w.noteOnCollapseThreshold > 1u, changed = false;
        ui::BeginTile("coalesce", "Combine rapid repeats", "Repeated hits of the same key within 20 ms share one voice.",
                      &on, &changed, tags, 2, 0.2f);
        if (changed) {
            w.noteOnCollapseThreshold = on ? 32u : 1u;
            doc.MarkDirty();
            SendLiveCommand(SVMS_COMMAND_SET_NOTE_ON_COLLAPSE, w.noteOnCollapseThreshold);
        }
        if (on) {
            ui::TileNested();
            static const uint32_t values[8] = {2u, 8u, 32u, 128u, 512u, 2048u, 8192u, 65536u};
            static const char* labels[8] = {"2", "8", "32", "128", "512", "2k", "8k", "64k"};
            int idx = -1;
            for (int i = 0; i < 8; ++i)
                if (values[i] == w.noteOnCollapseThreshold) idx = i;
            Nested n = NestedBegin("One voice per", nullptr, ui::SegmentedSize(labels, 8), true);
            if (ui::Segmented("##coalN", &idx, labels, 8, n.ctl.x) && idx >= 0) {
                w.noteOnCollapseThreshold = values[idx];
                doc.MarkDirty();
                SendLiveCommand(SVMS_COMMAND_SET_NOTE_ON_COLLAPSE, w.noteOnCollapseThreshold);
            }
            NestedEnd(n);
            n = NestedBegin("Keep the rhythm as swells",
                            "Each combined hit pushes the voice back to full level at its exact time. Slower.",
                            ImVec2(46.0f * s, 28.0f * s), false);
            if (ui::Toggle("##reexcite", &w.noteOnReexcite, 0.24f)) {
                doc.MarkDirty();
                SendLiveCommand(SVMS_COMMAND_SET_NOTE_ON_REEXCITE, w.noteOnReexcite ? 1u : 0u);
            }
            NestedEnd(n);
        }
        ui::EndTile();
    }
    {
        const ChipDef tags[] = {{"Note ends move", ChipKind::Bad}};
        char readout[24];
        if (w.minNoteMs) std::snprintf(readout, sizeof(readout), "%u ms", w.minNoteMs);
        else std::snprintf(readout, sizeof(readout), "Off");
        ui::BeginTile("minNote", "Minimum note length",
                      "Stretches very short notes so they are heard instead of vanishing into clicks.", nullptr, nullptr,
                      tags, 1, 0.0f, readout, w.minNoteMs > 0u);
        float v = static_cast<float>(w.minNoteMs);
        ui::RangeSlider("##minnote", &v, 0.0f, 100.0f, ImGui::GetContentRegionAvail().x);
        const uint32_t nv = static_cast<uint32_t>(v + 0.5f);
        if (nv != w.minNoteMs) {
            w.minNoteMs = nv;
            doc.MarkDirty();
            SendLiveCommand(SVMS_COMMAND_SET_MIN_NOTE_LENGTH, nv);
        }
        ui::EndTile();
    }
    ui::EndTileGrid();

    // --------------------------------------------- Costs accuracy or timing
    GroupHeader("Costs accuracy or timing", "Sounds the same to most ears, but no longer exact.", false);
    ui::BeginTileGrid("##g_acc");
    {
        const ChipDef tags[] = {{"Saves CPU", ChipKind::Cpu}, {"Not bit-exact", ChipKind::Bad}};
        SwitchTile(doc, "fastTail", "Fast tail math",
                   "Advances envelopes in 8-frame steps (AVX2). Peaks can differ from the exact path by up to \xE2\x88\x92" "48 dB.",
                   &w.fastChunkMath, SVMS_COMMAND_SET_FAST_CHUNK_MATH, tags, 2, 0.28f);
        ui::EndTile();
    }
    {
        const ChipDef tags[] = {{"Saves a lot of CPU", ChipKind::Cpu}, {"Bends can land a few ms late", ChipKind::Bad}};
        SwitchTile(doc, "bendCol", "Thin out pitch bends",
                   "Keeps only the latest bend per channel every few milliseconds. Note timing is untouched.",
                   &w.bendCollapse, SVMS_COMMAND_SET_BEND_COLLAPSE, tags, 2, 0.32f);
        ui::EndTile();
    }
    {
        const ChipDef tags[] = {{"Saves CPU", ChipKind::Cpu}, {"Timing snaps to the buffer", ChipKind::Bad}};
        SwitchTile(doc, "omni", "Block timing (OmniMIDI style)",
                   "Every event in an audio block fires at its start. Do not use for live playing.",
                   &w.blockTiming, SVMS_COMMAND_SET_BLOCK_TIMING, tags, 2, 0.36f);
        ui::EndTile();
    }
    {
        const ChipDef tags[] = {{"Experimental", ChipKind::Exp}};
        SwitchTile(doc, "unbounded", "Never skip ahead",
                   "Under overload the song slows down instead of jumping forward. Glitches are expected.",
                   &w.unboundedRender, SVMS_COMMAND_SET_UNBOUNDED_RENDER, tags, 1, 0.4f);
        ui::EndTile();
    }
    ui::EndTileGrid();
    ui::EndCard();
}

// --- CPU and threads ------------------------------------------------------------------

// Slider + mono readout as a row control.
bool SliderControl(const char* id, int* value, int minValue, int maxValue, const char* zeroText, ImVec2 size) {
    const float s = ui::Scale();
    const float rw = 48.0f * s;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + (size.y - 22.0f * s) * 0.5f));
    float v = static_cast<float>(*value);
    bool changed = false;
    if (ui::RangeSlider(id, &v, static_cast<float>(minValue), static_cast<float>(maxValue), size.x - rw - 10.0f * s)) {
        const int iv = static_cast<int>(v + 0.5f);
        if (iv != *value) {
            *value = iv;
            changed = true;
        }
    }
    char txt[16];
    if (*value == 0 && zeroText) std::snprintf(txt, sizeof(txt), "%s", zeroText);
    else std::snprintf(txt, sizeof(txt), "%d", *value);
    ImGui::SetCursorScreenPos(ImVec2(p.x + size.x - rw, p.y));
    ui::PushNum(13.0f);
    const ImVec2 ts = ImGui::CalcTextSize(txt);
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + size.x - ts.x, p.y + (size.y - ts.y) * 0.5f), ui::Tok().text, txt);
    ui::PopFont();
    ImGui::Dummy(ImVec2(rw, size.y));
    return changed;
}

void CpuCard(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const float s = ui::Scale();
    ui::BeginCard("##cpu", "CPU and threads", nullptr, 0.0f, "threads");
    {
        char desc[160];
        std::snprintf(desc, sizeof(desc), "How many CPU threads draw voices. Auto picks for you. This PC has %u.",
                      (std::max)(1u, std::thread::hardware_concurrency()));
        const ImVec2 size(258.0f * s, 28.0f * s);
        ui::BeginRow("threads", "Render threads", desc, size);
        int v = static_cast<int>(w.renderThreads);
        if (SliderControl("##threads", &v, 0, 64, "Auto", size)) {
            w.renderThreads = static_cast<uint32_t>(v);
            doc.MarkDirty();
        }
        ui::EndRow();
    }
    {
        const ImVec2 size(258.0f * s, 28.0f * s);
        ui::BeginRow("shards", "Engine shards",
                     "Splits the engine into independent parts by key. Helps very dense files. Keep threads at least twice the shard count.",
                     size);
        int v = static_cast<int>(w.shards);
        if (SliderControl("##shards", &v, 1, 16, nullptr, size)) {
            w.shards = static_cast<uint32_t>(v);
            doc.MarkDirty();
        }
        ui::EndRow();
        if (w.shards > 1u && w.renderThreads > 0u && w.renderThreads < w.shards * 2u)
            Wrapped("Fewer than two render threads per shard. Raise threads or lower shards.", ui::Tok().acc, 13.0f);
    }
    {
        static const char* labels[3] = {"Windows decides", "All on P-cores", "Audio on P-cores"};
        int idx = static_cast<int>((std::min)(2u, w.threadAffinityMode));
        ui::BeginRow("affinity", "Core placement", "For Intel CPUs with P- and E-cores. No effect on other CPUs.",
                     ui::SegmentedSize(labels, 3));
        if (ui::Segmented("##affinity", &idx, labels, 3)) {
            w.threadAffinityMode = static_cast<uint32_t>(idx);
            doc.MarkDirty();
            SendLiveCommand(SVMS_COMMAND_SET_THREAD_AFFINITY_MODE, w.threadAffinityMode);
        }
        ui::EndRow();
    }
    {
        ui::BeginRow("largePages", "Large memory pages",
                     "Faster with huge voice limits. Needs the \xE2\x80\x9CLock pages in memory\xE2\x80\x9D Windows privilege; quietly does nothing without it.",
                     ImVec2(46.0f * s, 28.0f * s));
        if (ui::Toggle("##largepages", &w.largePages)) doc.MarkDirty();
        ui::EndRow();
    }
    {
        ui::BeginRow("dropZeroLife", "Drop instant-steal blips (experimental)",
                     "A voice stolen on the same frame it started never plays a sample. This skips the short fade-out it would leave and lets those launches skip most of their work, which makes extreme black MIDIs far lighter on one shard. May change the sound slightly. Needs a restart.",
                     ImVec2(46.0f * s, 28.0f * s));
        if (ui::Toggle("##dropzerolife", &w.dropZeroLifeVictims)) doc.MarkDirty();
        ui::EndRow();
    }
    {
        static const char* labels[6] = {"Off", "Auto", "OpenGL", "Vulkan", "D3D11", "CUDA"};
        int idx = static_cast<int>((std::min)(5u, w.gpuApi));
        ui::BeginRow("gpuStage", "GPU rendering (experimental)",
                     "Renders steady sustained voices on the graphics card so a CPU can reach far higher voice counts. Falls back to the CPU on any problem. CUDA needs an NVIDIA card; OpenGL needs 4.3; Vulkan needs any Vulkan 1.1 card (D3D11 is not built yet); Auto tries CUDA, then OpenGL. Needs a restart.",
                     ui::SegmentedSize(labels, 6));
        if (ui::Segmented("##gpustage", &idx, labels, 6)) {
            w.gpuApi = static_cast<uint32_t>(idx);
            doc.MarkDirty();
        }
        ui::EndRow();
    }
    {
        ui::BeginRow("launchStaging", "Staged note launches (experimental)",
                     "Writes a stolen voice's replacement into one compact record and lets the render threads finish the copy, so the single planning thread does less work on extreme black MIDIs. The sound is bit-identical. Needs a restart.",
                     ImVec2(46.0f * s, 28.0f * s));
        if (ui::Toggle("##launchstaging", &w.launchStaging)) doc.MarkDirty();
        ui::EndRow();
    }
    ui::EndCard();
}

// --- Engine internals ------------------------------------------------------------------

void Internals(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const float s = ui::Scale();
    ui::BeginReveal("perf");
    ui::BeginCard("##internals");
    {
        const float width = ImGui::GetContentRegionAvail().x;
        const float kw = 96.0f * s;
        const float free = (std::max)(0.0f, width - 3.0f * kw) / 3.0f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float y = origin.y + 10.0f * s;
        float bottom = y;
        bool committed = false;

        float retireDb = 20.0f * std::log10((std::max)(1e-6f, w.voiceRetireThreshold));
        retireDb = ImClamp(retireDb, -100.0f, -26.0f);
        ImGui::SetCursorScreenPos(ImVec2(origin.x + free * 0.5f, y));
        const ui::KnobSpec retire = {"Retire floor", -100.0f, -26.0f, -55.0f, 1.0f, false, "%.0f dB"};
        if (ui::Knob("##retire", &retireDb, retire, &committed)) {
            w.voiceRetireThreshold = std::pow(10.0f, retireDb / 20.0f);
            doc.MarkDirty();
        }
        if (committed) {
            uint32_t bits = 0u;
            std::memcpy(&bits, &w.voiceRetireThreshold, sizeof(bits));
            SendLiveCommand(SVMS_COMMAND_SET_VOICE_RETIRE_FLOOR, bits);
        }
        ui::Anchor("retire", ImVec2(origin.x + free * 0.5f, y), ImVec2(origin.x + free * 0.5f + kw, y + ui::KnobSize().y));
        bottom = (std::max)(bottom, ImGui::GetCursorScreenPos().y);

        float cap = static_cast<float>(w.perKeyVoiceCap);
        ImGui::SetCursorScreenPos(ImVec2(origin.x + free * 1.5f + kw, y));
        const ui::KnobSpec perKey = {"Per-key cap", 0.0f, 32.0f, 0.0f, 1.0f, false, "%.0f / key", nullptr, "Off"};
        if (ui::Knob("##perkey", &cap, perKey, &committed)) {
            w.perKeyVoiceCap = static_cast<uint32_t>(cap + 0.5f);
            doc.MarkDirty();
        }
        if (committed) SendLiveCommand(SVMS_COMMAND_SET_PER_KEY_VOICE_CAP, w.perKeyVoiceCap);
        ui::Anchor("perKey", ImVec2(origin.x + free * 1.5f + kw, y), ImVec2(origin.x + free * 1.5f + 2.0f * kw, y + ui::KnobSize().y));
        bottom = (std::max)(bottom, ImGui::GetCursorScreenPos().y);

        float ghosts = static_cast<float>(w.ghostBudget);
        ImGui::SetCursorScreenPos(ImVec2(origin.x + free * 2.5f + 2.0f * kw, y));
        const ui::KnobSpec ghost = {"Ghost budget", 0.0f, 4096.0f, 0.0f, 1.0f, false, "%.0f", nullptr, "Off"};
        if (ui::Knob("##ghost", &ghosts, ghost, &committed)) {
            w.ghostBudget = static_cast<uint32_t>(ghosts + 0.5f);
            doc.MarkDirty();
        }
        if (committed) SendLiveCommand(SVMS_COMMAND_SET_GHOST_BUDGET, w.ghostBudget);
        ui::Anchor("ghost", ImVec2(origin.x + free * 2.5f + 2.0f * kw, y), ImVec2(origin.x + free * 2.5f + 3.0f * kw, y + ui::KnobSize().y));
        bottom = (std::max)(bottom, ImGui::GetCursorScreenPos().y);

        ImGui::SetCursorScreenPos(ImVec2(origin.x, bottom + 4.0f * s - ImGui::GetStyle().ItemSpacing.y));
        ImGui::Dummy(ImVec2(width, 0.0f));
        Wrapped("Retire floor: how quiet a release tail gets before it is freed. Per-key cap: max voices one key may hold. Ghost budget: cap on replaced voices finishing their fade per block.",
                ui::Tok().muted, 13.0f);
    }
    {
        ui::BeginRow("voiceMem", "Voice memory cap (MiB)", "Upper bound for voice storage. 0 = unlimited.",
                     ImVec2(150.0f * s, 38.0f * s));
        int v = static_cast<int>(w.voiceMemoryBudgetMB);
        if (ui::NumberField("##voicemem", &v, 0, 65536, 150.0f)) {
            w.voiceMemoryBudgetMB = static_cast<uint32_t>(v);
            doc.MarkDirty();
        }
        ui::EndRow();
    }
    ui::EndCard();
    ui::EndReveal();
}

} // namespace

void DrawPerformancePage(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const float s = ui::Scale();

    ui::BeginCard("##profile", "Profile", "Pick one. It sets the switches below for you.", 0.0f, "profile");
    ProfileCards(doc);
    if (MatchPerformanceProfile(w) < 0) {
        ImGui::Dummy(ImVec2(0.0f, 12.0f * s - ImGui::GetStyle().ItemSpacing.y));
        ui::Chip("Custom", ui::ChipKind::Acc);
        ImGui::SameLine(0.0f, 8.0f * s);
        const float dy = (ui::ChipSize("Custom").y - ImGui::GetTextLineHeight()) * 0.5f;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + dy);
        ui::PushText(0, 13.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::Tok().muted);
        ImGui::TextUnformatted("You changed individual switches below. Pick a profile to start over.");
        ImGui::PopStyleColor();
        ui::PopFont();
    }
    ui::EndCard();

    // Voice limit
    ui::BeginCard("##voicelimit", "Voice limit",
                  "How many notes can sound at once. When it is full, the quietest note is replaced.", 0.0f, "voices");
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ui::PushNum(40.0f, true);
        const std::string big = WithCommas(w.maxVoices);
        const ImVec2 bs = ImGui::CalcTextSize(big.c_str());
        dl->AddText(p, ui::Tok().text, big.c_str());
        ui::PopFont();
        ui::PushText(0, 13.0f);
        const ImVec2 vs = ImGui::CalcTextSize("voices");
        dl->AddText(ImVec2(p.x + bs.x + 10.0f * s, p.y + bs.y - vs.y - 4.0f * s), ui::Tok().muted, "voices");
        ui::PopFont();
        ImGui::Dummy(ImVec2(1.0f, bs.y + 14.0f * s - ImGui::GetStyle().ItemSpacing.y));
    }
    {
        static const int values[10] = {1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288};
        static const char* labels[10] = {"1k", "2k", "4k", "8k", "16k", "32k", "64k", "128k", "256k", "512k"};
        int idx = -1;
        for (int i = 0; i < 10; ++i)
            if (values[i] == static_cast<int>(w.maxVoices)) idx = i;
        if (ui::Segmented("##voices", &idx, labels, 10, -1.0f) && idx >= 0) {
            w.maxVoices = static_cast<uint32_t>(values[idx]);
            doc.MarkDirty();
            PushLiveMaxVoices(w.maxVoices);
        }
    }
    {
        // Any value, typed. Applied live when the field is left, not on every keystroke.
        ImGui::Dummy(ImVec2(0.0f, 10.0f * s - ImGui::GetStyle().ItemSpacing.y));
        ui::PushText(0, 13.0f);
        const ImVec2 row = ImGui::GetCursorScreenPos();
        const float lh = ImGui::GetTextLineHeight();
        ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + (38.0f * s - lh) * 0.5f));   // centre on the 38px field
        ImGui::TextUnformatted("Custom");
        const float cw = ImGui::CalcTextSize("Custom").x;
        ui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(row.x + cw + 12.0f * s, row.y));
        const int top = static_cast<int>(524288u * (w.shards > 1u ? w.shards : 1u));
        int custom = static_cast<int>(w.maxVoices);
        if (ui::NumberField("##voicescustom", &custom, 1, top, 150.0f)) {
            w.maxVoices = static_cast<uint32_t>(custom);
            doc.MarkDirty();
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) PushLiveMaxVoices(w.maxVoices);
    }
    if (w.maxVoices > 65536u) {
        ImGui::Dummy(ImVec2(0.0f, 10.0f * s - ImGui::GetStyle().ItemSpacing.y));
        Wrapped("Very high limits only pay off on a strong CPU. If playback stutters, go one step down or pick the Extreme profile.",
                ui::Tok().acc, 13.0f);
    }
    ui::EndCard();

    Shortcuts(doc);
    CpuCard(doc);

    if (ui::Disclosure("perf", "Engine internals", "Steal policy, retire floor, per-key cap, ghosts, memory cap"))
        Internals(doc);
}

} // namespace svms::cfg
