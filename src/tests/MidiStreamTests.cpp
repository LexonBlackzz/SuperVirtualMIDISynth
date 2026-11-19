#include "events/MidiStream.h"
#include "events/EventCompile.h"
#include "events/MidiArchive.h"
#include "events/MidiChunkStore.h"
#include "tests/MidiArchiveFixtures.h"
#if defined(_WIN32)
#include <windows.h>
#else
#include <codecvt>
#include <locale>
#include <unistd.h>
#endif
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {
bool Collect(const svms::PackedMidiEvent& e, void* user) {
    static_cast<std::vector<svms::PackedMidiEvent>*>(user)->push_back(e);
    return true;
}
bool WriteFixture(const wchar_t* path) {
    // Format 1, 480 PPQN. Track 0 establishes 120 BPM then 240 BPM at tick
    // 480. Track 1 uses running status for the second note-on.
    static const unsigned char bytes[] = {
        'M','T','h','d',0,0,0,6,0,1,0,2,1,0xe0,
        'M','T','r','k',0,0,0,19,
        0,0xff,0x51,3,7,0xa1,0x20,
        0x83,0x60,0xff,0x51,3,3,0xd0,0x90,
        0,0xff,0x2f,0,
        'M','T','r','k',0,0,0,43,
        0,0xf0,8,0x43,0x10,0x4c,0x00,0x00,0x06,0x58,0xf7,
        0,0x90,60,100,
        0,60,100,
        0,0xb0,7,127,
        0,0x90,60,100,
        0,60,100,
        0x83,0x60,0x90,255,100,
        0x83,0x60,0x80,60,0,
        0,0xff,0x2f,0
    };
#if defined(_WIN32)
    FILE* f = _wfopen(path,L"wb");
#else
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    const std::string utf8 = converter.to_bytes(path);
    FILE* f = fopen(utf8.c_str(), "wb");
#endif
    if(!f)return false;
    const bool ok=fwrite(bytes,1,sizeof(bytes),f)==sizeof(bytes);fclose(f);return ok;
}

// ---- chunk store tests ------------------------------------------------------

std::wstring TempPath(const wchar_t* name) {
#if defined(_WIN32)
    wchar_t dir[MAX_PATH]; GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + L"svms_midi_" + name;
#else
    return std::wstring(L"/tmp/svms_midi_") + name;
#endif
}
bool WriteBytes(const std::wstring& path, const std::vector<uint8_t>& bytes) {
#if defined(_WIN32)
    FILE* f = _wfopen(path.c_str(), L"wb");
#else
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    FILE* f = fopen(converter.to_bytes(path).c_str(), "wb");
#endif
    if (!f) return false;
    const bool ok = bytes.empty() || fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    fclose(f);
    return ok;
}
void RemoveFile(const std::wstring& path) {
#if defined(_WIN32)
    DeleteFileW(path.c_str());
#else
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    unlink(converter.to_bytes(path).c_str());
#endif
}
void PutVlq(std::vector<uint8_t>& o, uint32_t v) {
    uint8_t tmp[5]; int n = 0;
    tmp[n++] = uint8_t(v & 0x7f);
    while (v >>= 7) tmp[n++] = uint8_t(0x80 | (v & 0x7f));
    while (n) o.push_back(tmp[--n]);
}
void PutTrack(std::vector<uint8_t>& o, const std::vector<uint8_t>& body) {
    const uint32_t n = uint32_t(body.size());
    const uint8_t h[8] = {'M','T','r','k',uint8_t(n>>24),uint8_t(n>>16),uint8_t(n>>8),uint8_t(n)};
    o.insert(o.end(), h, h + 8);
    o.insert(o.end(), body.begin(), body.end());
}
std::vector<uint8_t> NoteBody(uint32_t seed, uint32_t notes) {
    std::vector<uint8_t> b;
    uint32_t x = seed;
    for (uint32_t i = 0; i < notes; ++i) {
        x = x * 1664525u + 1013904223u;
        const uint8_t key = uint8_t(36 + (x >> 24) % 48);
        PutVlq(b, (x >> 8) % 7); b.push_back(0x90); b.push_back(key); b.push_back(uint8_t(1 + (x >> 16) % 126));
        PutVlq(b, 1 + (x >> 4) % 30); b.push_back(key); b.push_back(0);   // running-status note-off
    }
    return b;
}
void EndOfTrack(std::vector<uint8_t>& b) { b.push_back(0); b.push_back(0xff); b.push_back(0x2f); b.push_back(0); }

// Deterministic format-1 MIDI: tempos, `noteTracks` note tracks built from
// `distinctBodies` different bodies (identical tracks repeat), a track with a
// 5000-byte SysEx between notes, CC and bend, and a track with trailing bytes
// after its End-of-Track (inside the MTrk length).
std::vector<uint8_t> BuildArchiveTestMidi(uint32_t noteTracks = 3, uint32_t distinctBodies = 1,
                                          uint32_t notes = 2000) {
    std::vector<uint8_t> o = {'M','T','h','d',0,0,0,6,0,1,0,0,1,0xe0};
    const uint32_t tracks = 3 + noteTracks;
    o[10] = uint8_t(tracks >> 8); o[11] = uint8_t(tracks);
    std::vector<uint8_t> t0 = {0,0xff,0x51,3,7,0xa1,0x20};
    PutVlq(t0, 1920); t0.insert(t0.end(), {0xff,0x51,3,3,0xd0,0x90});
    EndOfTrack(t0); PutTrack(o, t0);
    for (uint32_t i = 0; i < noteTracks; ++i) {
        std::vector<uint8_t> b = NoteBody(7 + i % distinctBodies, notes);
        EndOfTrack(b); PutTrack(o, b);
    }
    std::vector<uint8_t> t4 = NoteBody(99, notes * 3 / 20);
    t4.push_back(0); t4.push_back(0xf0); PutVlq(t4, 5000);
    for (uint32_t i = 0; i < 4999; ++i) t4.push_back(uint8_t(i % 0x7f));
    t4.push_back(0xf7);
    for (uint32_t i = 0; i < 200; ++i) {
        t4.insert(t4.end(), {3, 0xb1, 7, uint8_t(i % 128), 2, 0xe1, uint8_t(i % 128), 64});
    }
    std::vector<uint8_t> more = NoteBody(5, notes * 3 / 20);
    t4.insert(t4.end(), more.begin(), more.end());
    EndOfTrack(t4); PutTrack(o, t4);
    std::vector<uint8_t> t5 = NoteBody(1234, notes / 4);
    EndOfTrack(t5);
    t5.insert(t5.end(), {0x90, 1, 2, 3, 0xff, 0xff, 0x80});
    PutTrack(o, t5);
    return o;
}

struct Decoded {
    bool ok = false;
    std::string error;
    std::vector<svms::PackedMidiEvent> serial, parallel;
    svms::MidiStreamInfo scan{}, parallelInfo{};
    uint64_t frames = 0;
    std::vector<uint64_t> noteOnsPerTrack;
    std::vector<uint32_t> noteTracks;
    std::string stage;
};
Decoded DecodeAll(const std::wstring& path, const svms::MidiChunkParams* params) {
    Decoded d;
    svms::MappedMidiFile file;
    if (!file.Open(path.c_str(), d.error, params)) { d.stage = "open"; return d; }
    svms::MidiStreamDecoder decoder;
    std::atomic<bool> cancel{false};
    if (!decoder.Scan(file, 48000, d.scan, d.error)) { d.stage = "scan"; return d; }
    if (!decoder.Decode(file, 48000, Collect, &d.serial, &cancel, nullptr, d.error)) { d.stage = "decode"; return d; }
    svms::ParallelMidiDecoder parallel;
    if (!parallel.Decode(file, 48000, Collect, &d.parallel, &cancel, &d.parallelInfo, d.error,
                         nullptr, nullptr, 4)) { d.stage = "parallel"; return d; }
    if (!svms::ParallelMidiDecoder::MeasureDuration(file, 48000, d.frames, d.error, nullptr, 4,
                                                    &d.noteOnsPerTrack)) { d.stage = "measure"; return d; }
    if (!svms::ParallelMidiDecoder::NoteTracks(file, d.noteTracks, d.error)) { d.stage = "notetracks"; return d; }
    d.ok = true;
    return d;
}
bool SameEvents(const std::vector<svms::PackedMidiEvent>& a, const std::vector<svms::PackedMidiEvent>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].outputFrame != b[i].outputFrame || a[i].sequence != b[i].sequence ||
            a[i].message != b[i].message) return false;
    return true;
}
bool SameInfo(const svms::MidiStreamInfo& a, const svms::MidiStreamInfo& b) {
    return a.format == b.format && a.division == b.division && a.tracks == b.tracks &&
           a.eventCount == b.eventCount && a.noteOnCount == b.noteOnCount &&
           a.totalFrames == b.totalFrames && a.peakEventsPerSecond == b.peakEventsPerSecond &&
           a.peakNoteOnsPerSecond == b.peakNoteOnsPerSecond && a.peakEventsAtFrame == b.peakEventsAtFrame &&
           a.peakNoteOnsAtFrame == b.peakNoteOnsAtFrame &&
           a.peakExactDuplicateNoteOnsAtFrame == b.peakExactDuplicateNoteOnsAtFrame &&
           a.peakKeyDuplicateNoteOnsAtFrame == b.peakKeyDuplicateNoteOnsAtFrame &&
           a.exactDuplicateNoteOnCount == b.exactDuplicateNoteOnCount &&
           a.keyDuplicateNoteOnCount == b.keyDuplicateNoteOnCount &&
           a.noteRunExactDuplicateCount == b.noteRunExactDuplicateCount &&
           a.peakNoteRunExactDuplicatesAtFrame == b.peakNoteRunExactDuplicatesAtFrame &&
           a.adjacentExactDuplicateNoteOnCount == b.adjacentExactDuplicateNoteOnCount &&
           a.noteOnFrameCount == b.noteOnFrameCount && a.peakEventSecond == b.peakEventSecond &&
           a.peakNoteOnSecond == b.peakNoteOnSecond && a.peakFrame == b.peakFrame;
}
bool ExpectSameDecode(const std::wstring& a, const svms::MidiChunkParams* pa,
                      const std::wstring& b, const svms::MidiChunkParams* pb, const char* label) {
    const Decoded x = DecodeAll(a, pa), y = DecodeAll(b, pb);
    const char* what = nullptr;
    if (!x.ok) { std::printf("FAIL %s: reference %s failed: %s\n", label, x.stage.c_str(), x.error.c_str()); return false; }
    if (!y.ok) { std::printf("FAIL %s: %s failed: %s\n", label, y.stage.c_str(), y.error.c_str()); return false; }
    if (!SameEvents(x.serial, y.serial)) what = "serial events";
    else if (!SameEvents(x.parallel, y.parallel)) what = "parallel events";
    else if (!SameInfo(x.scan, y.scan)) what = "scan info";
    else if (!SameInfo(x.parallelInfo, y.parallelInfo)) what = "parallel info";
    else if (x.frames != y.frames) what = "measured duration";
    else if (x.noteOnsPerTrack != y.noteOnsPerTrack) what = "note-ons per track";
    else if (x.noteTracks != y.noteTracks) what = "note tracks";
    if (what) { std::printf("FAIL %s: %s differ\n", label, what); return false; }
    return true;
}
// Same failure, same stage, same text on both paths.
bool ExpectSameFailure(const std::wstring& path, const svms::MidiChunkParams& params, const char* label) {
    const Decoded x = DecodeAll(path, nullptr), y = DecodeAll(path, &params);
    if (x.ok || y.ok || x.stage != y.stage || x.error != y.error) {
        std::printf("FAIL %s: mapped [%s] %s vs store [%s] %s\n", label, x.stage.c_str(),
                    x.error.c_str(), y.stage.c_str(), y.error.c_str());
        return false;
    }
    return true;
}

bool TestForcedStoreMatchesMapped() {
    const std::wstring path = TempPath(L"store_a.mid");
    if (!WriteBytes(path, BuildArchiveTestMidi())) return false;
    svms::MidiChunkParams tiny; tiny.minChunk = 1; tiny.maxChunk = 1; tiny.mask = 0;
    svms::MidiChunkParams defaults;
    const bool ok = ExpectSameDecode(path, nullptr, path, &tiny, "forced store, one event per chunk") &&
                    ExpectSameDecode(path, nullptr, path, &defaults, "forced store, default chunks");
    RemoveFile(path);
    return ok;
}
bool TestStoreDedupsIdenticalTracks() {
    const std::wstring path = TempPath(L"store_b.mid");
    if (!WriteBytes(path, BuildArchiveTestMidi())) return false;
    svms::MidiChunkParams defaults;
    svms::MappedMidiFile file; std::string error;
    bool ok = file.Open(path.c_str(), error, &defaults) && file.Store();
    if (ok && !(double(file.Store()->StoredBytes()) < double(file.Store()->MidiBytes()) * 0.6)) {
        std::printf("FAIL dedup: stored %llu of %llu bytes\n",
                    (unsigned long long)file.Store()->StoredBytes(),
                    (unsigned long long)file.Store()->MidiBytes());
        ok = false;
    }
    file.Close();
    RemoveFile(path);
    if (!ok) std::printf("FAIL dedup %s\n", error.c_str());
    return ok;
}
bool TestStoreSysExIsOneChunk() {
    const std::wstring path = TempPath(L"store_c.mid");
    if (!WriteBytes(path, BuildArchiveTestMidi())) return false;
    svms::MidiChunkParams narrow; narrow.maxChunk = 64;
    svms::MappedMidiFile file; std::string error;
    bool found = false;
    if (file.Open(path.c_str(), error, &narrow) && file.Store()) {
        uint32_t count = 0;
        const uint32_t* refs = file.Store()->TrackRefs(4, count);
        for (uint32_t i = 0; i < count; ++i)
            if (file.Store()->Chunk(refs[i]).size >= 5000) found = true;
    }
    file.Close();
    RemoveFile(path);
    if (!found) std::printf("FAIL sysex chunk: no chunk of track 4 holds the whole SysEx\n");
    return found;
}
bool TestForcedStoreErrorsMatch() {
    svms::MidiChunkParams defaults;
    const std::wstring path = TempPath(L"store_d.mid");
    bool ok = WriteBytes(path, {'M','T','h','d',0,0,0,6,0,1}) &&
              ExpectSameFailure(path, defaults, "10-byte file");
    std::vector<uint8_t> lies = BuildArchiveTestMidi(1);   // 4 tracks
    lies[11] = 5;                                          // header claims 5
    ok = ok && WriteBytes(path, lies) && ExpectSameFailure(path, defaults, "header declares more tracks");
    std::vector<uint8_t> cut = BuildArchiveTestMidi(1);
    cut.resize(cut.size() - 40);                           // last track runs past the end
    ok = ok && WriteBytes(path, cut) && ExpectSameFailure(path, defaults, "truncated last track");
    RemoveFile(path);
    return ok;
}
uint64_t StoredBytesWith(const std::wstring& path, const svms::MidiChunkParams& params,
                         uint32_t* workers) {
    svms::MappedMidiFile file; std::string error;
    if (!file.Open(path.c_str(), error, &params) || !file.Store()) return 0;
    if (workers) *workers = file.Store()->WorkersUsed();
    return file.Store()->StoredBytes();
}
bool TestParallelStoreMatchesSerial() {
    const std::wstring path = TempPath(L"store_e.mid");
    if (!WriteBytes(path, BuildArchiveTestMidi(61, 3))) return false;   // 64 tracks
    svms::MidiChunkParams serial; serial.threads = 1;
    svms::MidiChunkParams pooled; pooled.threads = 8; pooled.pooledTrackMax = 1u << 20;
    svms::MidiChunkParams inline_; inline_.threads = 8; inline_.pooledTrackMax = 0;
    svms::MidiChunkParams tight; tight.threads = 8; tight.pooledTrackMax = 1u << 20; tight.inFlightMax = 8192;
    bool ok = ExpectSameDecode(path, &serial, path, &pooled, "pooled tracks on workers") &&
              ExpectSameDecode(path, &serial, path, &inline_, "every track on the reader") &&
              ExpectSameDecode(path, &serial, path, &tight, "tiny in-flight budget");
    uint32_t workers = 0;
    const uint64_t a = StoredBytesWith(path, serial, nullptr);
    const uint64_t b = StoredBytesWith(path, pooled, &workers);
    const uint64_t c = StoredBytesWith(path, inline_, nullptr);
    const uint64_t d = StoredBytesWith(path, tight, nullptr);
    if (ok && (a == 0 || a != b || a != c || a != d)) {
        std::printf("FAIL parallel store: stored bytes %llu %llu %llu %llu\n", (unsigned long long)a,
                    (unsigned long long)b, (unsigned long long)c, (unsigned long long)d);
        ok = false;
    }
    if (ok && workers <= 1) { std::printf("FAIL parallel store: %u workers used\n", workers); ok = false; }
    RemoveFile(path);
    return ok;
}
// ---- compressed files -------------------------------------------------------

template <size_t N>
std::vector<uint8_t> Bytes(const unsigned char (&a)[N]) { return std::vector<uint8_t>(a, a + N); }

// The fixture opens through a chunk store with the given format and decodes
// exactly like the plain MIDI it was made from.
bool ExpectFixtureDecodes(const std::vector<uint8_t>& bytes, const wchar_t* name,
                          const char* format, const std::vector<uint8_t>& plain) {
    const std::wstring plainPath = TempPath(L"plain.mid"), path = TempPath(name);
    bool ok = WriteBytes(plainPath, plain) && WriteBytes(path, bytes);
    if (ok) {
        svms::MappedMidiFile file; std::string error;
        if (!file.Open(path.c_str(), error)) {
            std::printf("FAIL %s: open failed: %s\n", format, error.c_str()); ok = false;
        } else if (!file.Store() || std::string(file.Format()) != format) {
            std::printf("FAIL %s: opened as '%s'%s\n", format, file.Format(),
                        file.Store() ? "" : " without a store");
            ok = false;
        }
    }
    ok = ok && ExpectSameDecode(plainPath, nullptr, path, nullptr, format);
    RemoveFile(plainPath); RemoveFile(path);
    return ok;
}
// Open fails; when `expected` is given the error text must be exactly that.
bool ExpectOpenFails(const std::vector<uint8_t>& bytes, const wchar_t* name, const char* label,
                     const char* expected = nullptr) {
    const std::wstring path = TempPath(name);
    if (!WriteBytes(path, bytes)) return false;
    svms::MappedMidiFile file; std::string error;
    const bool opened = file.Open(path.c_str(), error);
    file.Close();
    RemoveFile(path);
    if (opened || error.empty() || (expected && error != expected)) {
        std::printf("FAIL %s: open %s, error '%s'\n", label, opened ? "succeeded" : "failed",
                    error.c_str());
        return false;
    }
    return true;
}
std::vector<uint8_t> FixtureMidi() { return BuildArchiveTestMidi(3, 1, 200); }

bool TestXzLayer() { return ExpectFixtureDecodes(Bytes(kFixtureXz), L"a.mid.xz", "xz", FixtureMidi()); }
bool TestXzNested() {
    return ExpectFixtureDecodes(Bytes(kFixtureXzXz), L"a.mid.xz.xz", "xz+xz", FixtureMidi());
}
bool TestXzSplitStreams() {
    return ExpectFixtureDecodes(Bytes(kFixtureXzSplit), L"split.xz", "xz", FixtureMidi());
}
bool TestXzTruncated() { return ExpectOpenFails(Bytes(kFixtureXzTrunc), L"trunc.xz", "truncated xz"); }
bool TestRarRejected() {
    std::vector<uint8_t> rar = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00};
    rar.resize(32, 0);
    return ExpectOpenFails(rar, L"a.rar", "rar",
                           "RAR archives are not supported yet; extract the MIDI first");
}
bool TestUnknownStaysPlain() {
    const std::wstring path = TempPath(L"notes.txt");
    const char text[] = "this is not a MIDI file at all";
    if (!WriteBytes(path, std::vector<uint8_t>(text, text + sizeof(text) - 1))) return false;
    const Decoded d = DecodeAll(path, nullptr);
    RemoveFile(path);
    if (d.ok || d.stage != "scan" || d.error != "not a Standard MIDI File") {
        std::printf("FAIL unknown file: [%s] %s\n", d.stage.c_str(), d.error.c_str());
        return false;
    }
    return true;
}

bool TestGzipLayer() { return ExpectFixtureDecodes(Bytes(kFixtureGz), L"a.mid.gz", "gzip", FixtureMidi()); }
bool TestZstdLayer() { return ExpectFixtureDecodes(Bytes(kFixtureZst), L"a.mid.zst", "zstd", FixtureMidi()); }
bool TestGzipBadCrc() { return ExpectOpenFails(Bytes(kFixtureGzBadCrc), L"badcrc.gz", "gzip crc"); }
bool TestMixedNesting() {
    return ExpectFixtureDecodes(Bytes(kFixtureXzGz), L"a.mid.xz.gz", "gzip+xz", FixtureMidi());
}

bool TestZipPicksLargestMidi() {
    return ExpectFixtureDecodes(Bytes(kFixtureZipTwo), L"two.zip", "zip", FixtureMidi());
}
bool TestZipEntryNestedXz() {
    return ExpectFixtureDecodes(Bytes(kFixtureZipXz), L"xzin.zip", "zip+xz", FixtureMidi());
}
bool TestZipSniffsUnnamedEntry() {
    return ExpectFixtureDecodes(Bytes(kFixtureZipNoName), L"noname.zip", "zip", FixtureMidi());
}
bool TestZipNoMidi() {
    return ExpectOpenFails(Bytes(kFixtureZipNone), L"none.zip", "zip without a MIDI",
                           "no MIDI file found in the archive");
}
bool TestZipTruncated() { return ExpectOpenFails(Bytes(kFixtureZipTrunc), L"trunc.zip", "truncated zip"); }
bool TestPickMidiEntry() {
    using Entries = std::vector<std::pair<std::string, uint64_t>>;
    const bool ok = svms::PickMidiEntry(Entries{{"x.MID", 5}, {"y.midi.xz", 9}}) == 1 &&
                    svms::PickMidiEntry(Entries{{"a.txt", 9}}) == -1 &&
                    svms::PickMidiEntry(Entries{{"big.mid", 900}, {"small.mid", 10}, {"z.zst", 5000}}) == 0 &&
                    svms::PickMidiEntry(Entries{{"song.mid.gz", 3}, {"notes.midi.txt", 99}}) == 0;
    if (!ok) std::printf("FAIL PickMidiEntry\n");
    return ok;
}

int RunStoreTests() {
    if (!TestForcedStoreMatchesMapped()) return 20;
    if (!TestStoreDedupsIdenticalTracks()) return 21;
    if (!TestStoreSysExIsOneChunk()) return 22;
    if (!TestForcedStoreErrorsMatch()) return 23;
    if (!TestParallelStoreMatchesSerial()) return 24;
    if (!TestXzLayer()) return 30;
    if (!TestXzNested()) return 31;
    if (!TestXzSplitStreams()) return 32;
    if (!TestXzTruncated()) return 33;
    if (!TestRarRejected()) return 34;
    if (!TestUnknownStaysPlain()) return 35;
    if (!TestGzipLayer()) return 40;
    if (!TestZstdLayer()) return 41;
    if (!TestGzipBadCrc()) return 42;
    if (!TestMixedNesting()) return 43;
    if (!TestZipPicksLargestMidi()) return 50;
    if (!TestZipEntryNestedXz()) return 51;
    if (!TestZipSniffsUnnamedEntry()) return 52;
    if (!TestZipNoMidi()) return 53;
    if (!TestZipTruncated()) return 54;
    if (!TestPickMidiEntry()) return 55;
    return 0;
}
}
// Fixture source for the compressed-file tests: the archive fixtures are this
// output (and WriteFixture's) compressed by xz, gzip, zstd and 7-Zip.
int WriteFixtureMode(const std::wstring& mode, const std::wstring& path) {
    if (mode == L"--write-archive-midi") return WriteBytes(path, BuildArchiveTestMidi(3, 1, 200)) ? 0 : 1;
    if (mode == L"--write-small-midi") return WriteFixture(path.c_str()) ? 0 : 1;
    if (mode == L"--probe-decode") {   // decompression alone, output dropped
        const auto t0 = std::chrono::steady_clock::now();
        std::string format, error;
        std::unique_ptr<svms::ByteSource> src = svms::OpenMidiLayers(path.c_str(), format, error);
        if (!src) { std::printf("error: %s\n", error.empty() ? "not compressed" : error.c_str()); return 1; }
        std::vector<uint8_t> buf(4u << 20);
        uint64_t total = 0; size_t got = 0;
        while (src->Read(buf.data(), buf.size(), got, error) && got) total += got;
        if (!error.empty()) { std::printf("error: %s\n", error.c_str()); return 1; }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("format=%s decode_s=%.2f out_mb=%.1f\n", format.c_str(), s, total / 1e6);
        return 0;
    }
    if (mode == L"--probe") {
        const auto t0 = std::chrono::steady_clock::now();
        svms::MappedMidiFile file; std::string error;
        if (!file.Open(path.c_str(), error)) { std::printf("error: %s\n", error.c_str()); return 1; }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const svms::MidiChunkStore* st = file.Store();
        std::printf("format=%s load_s=%.2f store_mb=%.1f midi_mb=%.1f\n", file.Format(), s,
                    st ? st->StoredBytes() / 1e6 : 0.0, st ? st->MidiBytes() / 1e6 : file.Size() / 1e6);
        return 0;
    }
    return -1;
}
#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    if (argc == 3) return WriteFixtureMode(argv[1], argv[2]);
    wchar_t dir[MAX_PATH],path[MAX_PATH];GetTempPathW(MAX_PATH,dir);GetTempFileNameW(dir,L"svm",0,path);
#else
int main(int argc, char** argv) {
    if (argc == 3) {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> args;
        return WriteFixtureMode(args.from_bytes(argv[1]), args.from_bytes(argv[2]));
    }
    char temporary[] = "/tmp/svms-midi-stream-XXXXXX";
    const int descriptor = mkstemp(temporary);
    if (descriptor < 0) return 1;
    close(descriptor);
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    const std::wstring widePath = converter.from_bytes(temporary);
    const wchar_t* path = widePath.c_str();
#endif
    if(!WriteFixture(path))return 1;
    svms::MappedMidiFile file;std::string error;if(!file.Open(path,error)){
#if defined(_WIN32)
        DeleteFileW(path);
#else
        unlink(temporary);
#endif
        return 2;
    }
    svms::MidiStreamDecoder decoder;svms::MidiStreamInfo info{};
    if(!decoder.Scan(file,48000,info,error)){
#if defined(_WIN32)
        DeleteFileW(path);
#else
        unlink(temporary);
#endif
        return 3;
    }
    if(info.eventCount!=8||info.noteOnCount!=5||info.totalFrames!=36000||info.format!=1||info.tracks!=2)return 4;
    if(info.peakEventsPerSecond!=8||info.peakNoteOnsPerSecond!=5||
       info.peakEventsAtFrame!=6||info.peakNoteOnsAtFrame!=4||
       info.exactDuplicateNoteOnCount!=3||info.keyDuplicateNoteOnCount!=3||
       info.peakExactDuplicateNoteOnsAtFrame!=3||
       info.peakKeyDuplicateNoteOnsAtFrame!=3||
       info.noteRunExactDuplicateCount!=2||
       info.peakNoteRunExactDuplicatesAtFrame!=2||
       info.adjacentExactDuplicateNoteOnCount!=2||info.noteOnFrameCount!=2)return 13;
    std::vector<svms::PackedMidiEvent> events;std::atomic<bool> cancel{false};
    if(!decoder.Decode(file,48000,Collect,&events,&cancel,nullptr,error))return 5;
#if defined(_WIN32)
    DeleteFileW(path);
#else
    unlink(temporary);
#endif
    if(events.size()!=8)return 6;
    if(events[0].outputFrame!=0||events[1].outputFrame!=0||
       events[2].outputFrame!=0||events[3].outputFrame!=0||
       events[4].outputFrame!=0||events[5].outputFrame!=0||
       events[6].outputFrame!=24000||events[7].outputFrame!=36000)return 7;
    if(events[0].message!=svms::MakeInternalMasterTransposeMessage(0x58)||
       events[1].message!=0x00643c90||events[2].message!=0x00643c90||
       events[3].message!=0x007f07b0||events[4].message!=0x00643c90||
       events[5].message!=0x00643c90||events[6].message!=0x0064ff90||
       events[7].message!=0x00003c80)return 8;
    svms::ParsedEventRing ring(1);if(!ring.IsValid()||ring.Capacity()!=65536)return 9;
    for(const auto& e:events)if(!ring.Push(e,cancel))return 10;
    svms::PackedMidiEvent e{};for(const auto& expected:events){if(!ring.Pop(e)||e.sequence!=expected.sequence)return 11;}
    if(ring.Size()!=0)return 12;
    return RunStoreTests();
}
