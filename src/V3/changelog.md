# SVMS V3 — running changelog

Low-key running log, updated with every landed change. The root
`CHANGELOG.md` carries the polished per-version entries I publish when I cut
a release; this file is where changes land first, raw, under "Unreleased",
until I move them into a release section.

Format: newest first, one bullet per landed change, matching the commit's
`type(v3): one-liner` style plus a line of context where it helps.

## Unreleased

- 2026-09-09 fix(v3): BASSMIDI StreamCreate first parameter is the MIDI
  channel count, not output channels — Kiva's 16-channel request now passes
  and streams always render the stereo pair (was rejected with a mislabeled
  BUFLOST error code).
- 2026-09-09 fix(v3): BASSMIDI shim surfaces Kiva — added BASS_MIDI_FontLoad,
  BASS_ChannelFlags and BASS_GetVersion exports (host + forwarder), and
  corrected BASS_MIDI_StreamEvents to the real 4-arg bassmidi ABI
  (handle, mode, events, length) with TIME/tick/RAW/CANCEL mode parsing;
  render pulls now report BASS_ERROR_ENDED (45) past the event tail so
  prerender pumps terminate.
