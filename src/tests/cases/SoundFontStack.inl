// Stack-wide preset resolution (synth/PresetResolve.h): an SFZ must not
// swallow every program, and per-channel assignment overrides priority.

namespace {
std::unique_ptr<svms::SF2Data> MakeStackFont(
    bool sfz, std::initializer_list<std::pair<uint16_t, uint16_t>> presets) {
    std::unique_ptr<svms::SF2Data> data(new svms::SF2Data());
    data->isSfz = sfz;
    for (const auto& p : presets) {
        data->presets[data->presetCount].bank = p.first;
        data->presets[data->presetCount].preset = p.second;
        ++data->presetCount;
    }
    return data;
}

bool ResolveFor(const std::vector<const svms::SF2Data*>& banks, int channelFont,
                uint16_t bank, uint8_t program, bool percussion,
                uint8_t& font) {
    uint32_t preset = 0u;
    return svms::ResolveStackPreset(banks.data(),
        static_cast<uint32_t>(banks.size()), nullptr, 0u, channelFont, bank,
        program, percussion, &font, &preset);
}
}  // namespace

void TestSoundFontStackResolution() {
    auto gm = MakeStackFont(false, {{0, 0}, {0, 1}, {0, 40}, {128, 0}});
    auto piano = MakeStackFont(false, {{0, 0}});
    auto sfz = MakeStackFont(true, {{0, 0}});
    uint8_t font = 99u;

    // SFZ first in the stack: it keeps piano but does not take other programs.
    std::vector<const svms::SF2Data*> sfzFirst{sfz.get(), gm.get()};
    Check(ResolveFor(sfzFirst, -1, 0, 0, false, font) && font == 0u,
          "stack: SFZ answers Acoustic Grand");
    Check(ResolveFor(sfzFirst, -1, 0, 40, false, font) && font == 1u,
          "stack: SFZ listed first does not swallow program 40");
    Check(ResolveFor(sfzFirst, -1, 0, 0, true, font) && font == 1u,
          "stack: SFZ listed first does not take the drum channel");

    // Nothing else has the program: the SFZ is the last resort.
    std::vector<const svms::SF2Data*> pianoThenSfz{piano.get(), sfz.get()};
    Check(ResolveFor(pianoThenSfz, -1, 0, 9, false, font) && font == 1u,
          "stack: SFZ is the last resort for an unanswered program");
    Check(ResolveFor(pianoThenSfz, -1, 0, 0, false, font) && font == 0u,
          "stack: earlier SF2 beats the SFZ for a program both have");

    // A lone SFZ keeps answering everything, as before.
    std::vector<const svms::SF2Data*> lone{sfz.get()};
    Check(ResolveFor(lone, -1, 0, 73, false, font) && font == 0u &&
              ResolveFor(lone, -1, 0, 0, true, font) && font == 0u,
          "single SFZ still answers every program and the drum channel");

    // A piano-only SF2 does not claim other programs.
    std::vector<const svms::SF2Data*> pianoThenGm{piano.get(), gm.get()};
    Check(ResolveFor(pianoThenGm, -1, 0, 40, false, font) && font == 1u,
          "stack: piano-only SF2 falls through to the next SoundFont");

    // Per-channel assignment overrides priority, and falls back when the
    // assigned SoundFont lacks the program.
    Check(ResolveFor(pianoThenGm, 1, 0, 0, false, font) && font == 1u,
          "channel assignment beats stack priority");
    Check(ResolveFor(pianoThenGm, 0, 0, 40, false, font) && font == 1u,
          "assigned SoundFont without the program falls back to the stack");
}
