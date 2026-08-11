# V3 Offline Renderer

`svms_v3_render` renders a Standard MIDI File through the same SF2 voice
manager and runtime-selected scalar/SSE2/AVX2 kernels used by V3. It does not
alter or replace the real-time `winmm.dll` path.

```bat
build\V3\bin\svms_v3_render.exe input.mid soundfont.sf2 output.wav
```

The input MIDI is memory-mapped rather than copied into RAM. A background
decoder merges format-1 tracks and converts ticks/tempo to exact integer output
frames. Decoded channel events pass through a fixed 128 MiB ring by default;
the ring holds 8,388,608 events and rotates for larger files. Audio is written
incrementally as stereo 32-bit float WAV, or RF64 when the estimated result
cannot fit RIFF's 32-bit size fields.

Useful options:

```text
--sample-rate N
--max-voices N
--event-buffer-mb N
--block-frames N
--tail-seconds N
--master-volume F
--backend auto|scalar|sse2|avx2
--scan-only
--quiet
```

Progress is printed once per second with rendered/total time, active and free
voices, exact voice steals, dispatched events, render speed, milliseconds per
audio second, and ETA. `--scan-only` validates a MIDI and reports channel-event
and note-on counts without loading the SoundFont or creating output.

It may also be used without dummy output arguments:

```bat
build\V3\bin\svms_v3_render.exe input.mid --scan-only
```

At end of file, outstanding voices receive channel-local all-notes-off and
their natural SF2 release tails render for up to `--tail-seconds` (30 by
default). The output is deterministic for a fixed build, backend, SoundFont,
and command line.
