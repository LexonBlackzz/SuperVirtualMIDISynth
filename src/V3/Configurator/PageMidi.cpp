#include "PageMidi.h"
#include "ConfigDocument.h"
#include "Widgets.h"
#include "imgui.h"
#include "../SVMSRuntimeLinkProtocol.h"
#include <cmath>

namespace svms::cfg {

void DrawMidiPage(ConfigDocument& doc) {
    auto& w = doc.Working();

    SectionHeader("SYNTH SETTINGS");

    float masterVol = w.masterVolume;
    if (SliderFloat("Master Volume", &masterVol, 0.0f, 4.0f, "%.2f",
                    "Master output volume multiplier. 1.0 is unity gain.")) {
        w.masterVolume = masterVol;
        doc.MarkDirty();
        PushLiveFloat(svms::RLCommandType::SetMasterVolume, masterVol);
    }
    {
        auto& lc = GetLiveLinkContext();
        if (lc.connected) LiveBadge("Applied live via RuntimeLink");
        AppliedStateBadge(lc.connected, lc.telemetry, w,
                          "Master-volume applied state vs working copy");
    }
    ImGui::TextDisabled("%.1f dB", 20.0f * std::log10(std::max(masterVol, 0.001f)));

    float velCurve = w.velocityCurve;
    if (SliderFloat("Velocity Curve", &velCurve, 0.1f, 10.0f, "%.2f",
                    "Exponent applied to MIDI velocity. >1 boosts loud notes, "
                    "<1 boosts quiet notes.")) {
        w.velocityCurve = velCurve;
        doc.MarkDirty();
    }

    float velFloor = w.velocityFloor;
    if (SliderFloat("Velocity Floor", &velFloor, 0.0f, 0.99f, "%.2f",
                    "Velocities below this fraction of max are attenuated. "
                    "Useful for shedding very quiet events under load.")) {
        w.velocityFloor = velFloor;
        doc.MarkDirty();
    }

    int velIgnore = static_cast<int>(w.velocityIgnoreBelow);
    if (LabeledInt("Ignore velocity below", &velIgnore, 0, 127,
                   "MIDI notes with velocity at or below this value are "
                   "silently discarded.")) {
        w.velocityIgnoreBelow = static_cast<uint32_t>(velIgnore);
        doc.MarkDirty();
    }

    ImGui::Spacing();
    SectionHeader("EVENT QUEUE");

    static const char* overflowItems[] = { "Priority", "Lossless" };
    int overflowIdx = w.overflowMode;
    if (LabeledCombo("Overflow Mode", &overflowIdx, overflowItems, 2,
                     "Priority: quiet note-ons may be discarded under severe "
                     "queue pressure. Lossless: favors backpressure rather "
                     "than event shedding.")) {
        w.overflowMode = overflowIdx;
        doc.MarkDirty();
    }

    int ringCap = static_cast<int>(w.eventRingCapacity);
    if (LabeledInt("Ring Capacity", &ringCap, 4096, 1048576,
                   "Size of the MIDI event ring buffer. Larger values "
                   "absorb longer bursts but use more memory.")) {
        w.eventRingCapacity = static_cast<uint32_t>(ringCap);
        doc.MarkDirty();
    }

    int hiPriVel = static_cast<int>(w.highPriorityVelocity);
    if (LabeledInt("High-Priority Velocity", &hiPriVel, 1, 127,
                   "MIDI note-ons at or above this velocity are protected "
                   "from priority shedding.")) {
        w.highPriorityVelocity = static_cast<uint32_t>(hiPriVel);
        doc.MarkDirty();
    }

    int shedPct = static_cast<int>(w.shedStartPercent);
    if (LabeledInt("Shed Start Percent", &shedPct, 1, 99,
                   "Queue fill percentage at which priority shedding "
                   "begins activating.")) {
        w.shedStartPercent = static_cast<uint32_t>(shedPct);
        doc.MarkDirty();
    }

    int maxEvts = static_cast<int>(w.maxEventsPerBlock);
    if (LabeledInt("Max Events Per Block", &maxEvts, 1, 1048576,
                   "Maximum MIDI events processed per audio callback. "
                   "Cannot exceed ring capacity.")) {
        w.maxEventsPerBlock = static_cast<uint32_t>(maxEvts);
        doc.MarkDirty();
    }

    ImGui::Spacing();
    SectionHeader("MIDI INPUT");

    // The driver ships winmm's midiIn* entry points for drop-in
    // compatibility, but input routing to the synth is not implemented —
    // they return MMSYSERR_BADDEVICEID.  Nothing to configure here yet.
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.56f, 0.59f, 0.62f, 1.0f));
    ImGui::TextWrapped(
        "MIDI In is not implemented: the winmm compatibility layer "
        "exposes midiIn* entry points that return MMSYSERR_BADDEVICEID. "
        "External controllers and virtual MIDI cables cannot drive the "
        "synth yet.");
    ImGui::PopStyleColor();
}

} // namespace svms::cfg
