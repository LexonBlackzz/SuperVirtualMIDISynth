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
        "Every note exactly as written.",
        "Only shortcuts you cannot hear.",
        "Every shortcut on, for Black MIDI."};
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

// Slider + mono readout as a row control. `committed` is set when a drag ends.
bool SliderControl(const char* id, int* value, int minValue, int maxValue, const char* zeroText, ImVec2 size,
                   bool* committed = nullptr) {
    const float s = ui::Scale();
    const float rw = 48.0f * s;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + (size.y - 22.0f * s) * 0.5f));
    float v = static_cast<float>(*value);
    bool changed = false;
    if (ui::RangeSlider(id, &v, static_cast<float>(minValue), static_cast<float>(maxValue), size.x - rw - 10.0f * s,
                        committed)) {
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

// --- Shortcuts ---------------------------------------------------------------------

using ui::ChipDef;
using ui::ChipKind;

// A row whose toggle flips a bool and pushes a live command.
void SwitchRow(ConfigDocument& doc, const char* anchor, const char* title, const char* desc, bool* value,
               uint32_t command, const ChipDef* chips, int chipCount, float delay) {
    const float s = ui::Scale();
    ui::BeginRow(anchor, title, desc, ImVec2(46.0f * s, 28.0f * s), chips, chipCount);
    if (ui::Toggle(anchor, value, delay)) {
        doc.MarkDirty();
        SendLiveCommand(command, *value ? 1u : 0u);
    }
    ui::EndRow();
}

void Shortcuts(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const float s = ui::Scale();
    const ImVec2 toggle(46.0f * s, 28.0f * s);
    const ImVec2 slider(258.0f * s, 28.0f * s);

    ui::BeginCard("##sc_free", "Free shortcuts", "No audible difference.");
    {
        const ChipDef c[] = {{"Saves voices", ChipKind::Cpu}};
        SwitchRow(doc, "noteMerge", "Merge identical notes", "Same key, velocity and time on several tracks plays as one voice.",
                  &w.noteMerge, SVMS_COMMAND_SET_NOTE_MERGE, c, 1, 0.0f);
    }
    {
        const ChipDef c[] = {{"Saves CPU", ChipKind::Cpu}};
        SwitchRow(doc, "ccCol", "Skip overwritten controller changes", "Drops controller changes replaced within a few ms.",
                  &w.ccCollapse, SVMS_COMMAND_SET_CC_COLLAPSE, c, 1, 0.04f);
    }
    ui::EndCard();

    ui::BeginCard("##sc_sound", "Shortcuts that cost a little sound", "Big savings on dense Black MIDI.");
    {
        const ChipDef c[] = {{"Saves a lot of CPU", ChipKind::Cpu}, {"Thins quiet layers", ChipKind::Bad}};
        ui::BeginRow("decim", "Voice decimation", "Under heavy load, only the loudest voices are mixed.", toggle, c, 2);
        if (ui::Toggle("##decim", &w.decimation, 0.08f)) {
            doc.MarkDirty();
            SendLiveCommand(SVMS_COMMAND_SET_DECIMATION, DecimationParam(w));
        }
        ui::EndRow();
        if (w.decimation) {
            ui::BeginNest();
            ui::BeginRow("decByLevel", "Mute by actual loudness", "Fading tails go first, not soft new notes.", toggle);
            if (ui::Toggle("##bylevel", &w.decimationByLevel, 0.12f)) {
                doc.MarkDirty();
                SendLiveCommand(SVMS_COMMAND_SET_DECIMATION, DecimationParam(w));
            }
            ui::EndRow();
            ui::BeginRow("cull", "Free muted tails after (blocks)", nullptr, slider);
            int cull = static_cast<int>(w.decimationCullBlocks);
            bool committed = false;
            if (SliderControl("##cull", &cull, 0, 120, "Never", slider, &committed)) {
                w.decimationCullBlocks = static_cast<uint32_t>(cull);
                doc.MarkDirty();
            }
            if (committed) SendLiveCommand(SVMS_COMMAND_SET_DECIMATION, DecimationParam(w));
            ui::EndRow();
            ui::EndNest();
        }
    }
    {
        const ChipDef c[] = {{"Saves CPU", ChipKind::Cpu}, {"Drops quiet notes", ChipKind::Bad}};
        SwitchRow(doc, "stealReject", "Skip notes quieter than everything playing",
                  "When voices are full, a note that would be replaced first is not played.",
                  &w.stealReject, SVMS_COMMAND_SET_STEAL_REJECT, c, 2, 0.16f);
    }
    {
        const ChipDef c[] = {{"Saves CPU", ChipKind::Cpu}, {"Loses retriggers", ChipKind::Bad}};
        bool on = w.noteOnCollapseThreshold > 1u;
        ui::BeginRow("coalesce", "Combine rapid repeats", "Hits on one key within 20 ms share a voice.", toggle, c, 2);
        if (ui::Toggle("##coalesce", &on, 0.2f)) {
            w.noteOnCollapseThreshold = on ? 32u : 1u;
            doc.MarkDirty();
            SendLiveCommand(SVMS_COMMAND_SET_NOTE_ON_COLLAPSE, w.noteOnCollapseThreshold);
        }
        ui::EndRow();
        if (on) {
            ui::BeginNest();
            static const uint32_t values[8] = {2u, 8u, 32u, 128u, 512u, 2048u, 8192u, 65536u};
            static const char* labels[8] = {"2", "8", "32", "128", "512", "2k", "8k", "64k"};
            int idx = -1;
            for (int i = 0; i < 8; ++i)
                if (values[i] == w.noteOnCollapseThreshold) idx = i;
            ui::BeginRow(nullptr, "Hits per voice", nullptr, ui::SegmentedSize(labels, 8));
            if (ui::Segmented("##coalN", &idx, labels, 8) && idx >= 0) {
                w.noteOnCollapseThreshold = values[idx];
                doc.MarkDirty();
                SendLiveCommand(SVMS_COMMAND_SET_NOTE_ON_COLLAPSE, w.noteOnCollapseThreshold);
            }
            ui::EndRow();
            ui::BeginRow("reexcite", "Keep the rhythm as swells", "Each hit swells the voice back up. Slower.", toggle);
            if (ui::Toggle("##reexcite", &w.noteOnReexcite, 0.24f)) {
                doc.MarkDirty();
                SendLiveCommand(SVMS_COMMAND_SET_NOTE_ON_REEXCITE, w.noteOnReexcite ? 1u : 0u);
            }
            ui::EndRow();
            ui::EndNest();
        }
    }
    {
        const ChipDef c[] = {{"Note ends move", ChipKind::Bad}};
        ui::BeginRow("minNote", "Minimum note length (ms)", "Stretches very short notes so they are heard.", slider, c, 1);
        int v = static_cast<int>(w.minNoteMs);
        if (SliderControl("##minnote", &v, 0, 100, "Off", slider)) {
            w.minNoteMs = static_cast<uint32_t>(v);
            doc.MarkDirty();
            SendLiveCommand(SVMS_COMMAND_SET_MIN_NOTE_LENGTH, w.minNoteMs);
        }
        ui::EndRow();
    }
    ui::EndCard();

    ui::BeginCard("##sc_exact", "Shortcuts that cost exactness", "Most ears will not tell.");
    {
        const ChipDef c[] = {{"Saves CPU", ChipKind::Cpu}, {"Not bit-exact", ChipKind::Bad}};
        SwitchRow(doc, "fastTail", "Fast tail math", "Envelopes advance in 8-frame steps (AVX2).",
                  &w.fastChunkMath, SVMS_COMMAND_SET_FAST_CHUNK_MATH, c, 2, 0.28f);
        SwitchRow(doc, "halfRate", "Half-rate voices", "Steady, dull voices render at half the sample rate.",
                  &w.halfRate, SVMS_COMMAND_SET_HALF_RATE, c, 2, 0.3f);
    }
    {
        const ChipDef c[] = {{"Saves a lot of CPU", ChipKind::Cpu}, {"Bends a few ms late", ChipKind::Bad}};
        SwitchRow(doc, "bendCol", "Thin out pitch bends", "Keeps the latest bend every few ms. Notes stay on time.",
                  &w.bendCollapse, SVMS_COMMAND_SET_BEND_COLLAPSE, c, 2, 0.32f);
    }
    {
        const ChipDef c[] = {{"Saves CPU", ChipKind::Cpu}, {"Timing snaps to blocks", ChipKind::Bad}};
        SwitchRow(doc, "omni", "Block timing (OmniMIDI style)", "Events fire at the start of each audio block. Not for live play.",
                  &w.blockTiming, SVMS_COMMAND_SET_BLOCK_TIMING, c, 2, 0.36f);
    }
    {
        const ChipDef c[] = {{"Experimental", ChipKind::Exp}};
        SwitchRow(doc, "unbounded", "Never skip ahead", "Under overload the song slows down instead of jumping ahead.",
                  &w.unboundedRender, SVMS_COMMAND_SET_UNBOUNDED_RENDER, c, 1, 0.4f);
    }
    ui::EndCard();
}

// --- CPU and threads ------------------------------------------------------------------

void CpuCard(ConfigDocument& doc) {
    ConfigValues& w = doc.Working();
    const float s = ui::Scale();
    ui::BeginCard("##cpu", "CPU and threads", nullptr, 0.0f, "threads");
    {
        char desc[160];
        std::snprintf(desc, sizeof(desc), "Threads that draw voices. This PC has %u.",
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
                     "Splits the engine by key, for very dense files.",
                     size);
        int v = static_cast<int>(w.shards);
        if (SliderControl("##shards", &v, 1, 16, nullptr, size)) {
            w.shards = static_cast<uint32_t>(v);
            doc.MarkDirty();
        }
        ui::EndRow();
        if (w.shards > 1u && w.renderThreads > 0u && w.renderThreads < w.shards * 2u)
            Wrapped("Use at least two render threads per shard.", ui::Tok().acc, 13.0f);
    }
    {
        static const char* labels[3] = {"Windows decides", "All on P-cores", "Audio on P-cores"};
        int idx = static_cast<int>((std::min)(2u, w.threadAffinityMode));
        ui::BeginRow("affinity", "Core placement", "For Intel CPUs with P- and E-cores.",
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
                     "Faster at huge voice limits. Needs the \xE2\x80\x9CLock pages in memory\xE2\x80\x9D privilege.",
                     ImVec2(46.0f * s, 28.0f * s));
        if (ui::Toggle("##largepages", &w.largePages)) doc.MarkDirty();
        ui::EndRow();
    }
    {
        ui::BeginRow("keepLoaded", "Keep the engine loaded",
                     "Stays loaded when a program closes SVMS, so the next one starts at once.",
                     ImVec2(46.0f * s, 28.0f * s));
        if (ui::Toggle("##keeploaded", &w.keepLoaded)) doc.MarkDirty();
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
        Wrapped("Retire floor: when a fading tail is freed. Per-key cap: voices one key may hold. Ghost budget: replaced voices still fading, per block.",
                ui::Tok().muted, 13.0f);
    }
    {
        ui::BeginRow("voiceMem", "Voice memory cap (MiB)", "0 = unlimited.",
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

    ui::BeginCard("##profile", "Profile", "Sets the switches below.", 0.0f, "profile");
    ProfileCards(doc);
    if (MatchPerformanceProfile(w) < 0) {
        ImGui::Dummy(ImVec2(0.0f, 12.0f * s - ImGui::GetStyle().ItemSpacing.y));
        ui::Chip("Custom", ui::ChipKind::Acc);
        ImGui::SameLine(0.0f, 8.0f * s);
        const float dy = (ui::ChipSize("Custom").y - ImGui::GetTextLineHeight()) * 0.5f;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + dy);
        ui::PushText(0, 13.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::Tok().muted);
        ImGui::TextUnformatted("You changed switches below.");
        ImGui::PopStyleColor();
        ui::PopFont();
    }
    ui::EndCard();

    // Voice limit
    ui::BeginCard("##voicelimit", "Voice limit",
                  "How many notes can sound at once.", 0.0f, "voices");
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
        Wrapped("Limits this high need a strong CPU.",
                ui::Tok().acc, 13.0f);
    }
    ui::EndCard();

    Shortcuts(doc);
    CpuCard(doc);

    if (ui::Disclosure("perf", "Engine internals", "Retire floor, per-key cap, ghosts, memory cap"))
        Internals(doc);
}

} // namespace svms::cfg
