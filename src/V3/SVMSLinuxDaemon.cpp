#include "SVMSMPSCQueue.h"
#include "SVMSStandaloneSynth.h"

#include <alsa/asoundlib.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <codecvt>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <locale>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <time.h>

namespace {

constexpr uint32_t kMidiQueueCapacity = 262144u;

volatile std::sig_atomic_t g_stop = 0;

void SignalHandler(int) { g_stop = 1; }

uint64_t MonotonicNanoseconds() {
    timespec value{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &value);
    return uint64_t(value.tv_sec) * 1000000000ull + uint64_t(value.tv_nsec);
}

struct Options {
    std::wstring soundfont;
    std::string pcmDevice = "default";
    std::string clientName = "SuperVirtualMIDISynth V3";
    uint32_t sampleRate = 44100u;
    uint32_t bufferFrames = 2048u;
    uint32_t maxVoices = 4096u;
    uint32_t renderThreads = 0u;
    float masterVolume = 1.0f;
    bool limiterEnabled = true;
    svms::RenderBackend backend = svms::RenderBackend::AVX512;
};

void Usage() {
    std::fputs(
        "SuperVirtualMIDISynth V3 Linux ALSA daemon\n\n"
        "svmsd --soundfont FILE [options]\n\n"
        "  --audio-device NAME   ALSA PCM device (default: default)\n"
        "  --client-name NAME    ALSA Sequencer client name\n"
        "  --sample-rate N       8000-384000 (default: 44100)\n"
        "  --buffer-frames N     Render quantum, 16-8192 (default: 2048)\n"
        "  --max-voices N        1-524288 (default: 4096)\n"
        "  --render-threads N    Total render threads, 0-64; 0=auto\n"
        "  --master-volume F     Linear gain, 0-4 (default: 1)\n"
        "  --backend auto|scalar|sse2|avx2\n"
        "  --no-limiter          Disable the output limiter\n"
        "  --help                 Show this help\n",
        stdout);
}

bool ParseU32(const char* text, uint32_t minimum, uint32_t maximum,
              uint32_t& output) {
    if (!text || !*text) return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (errno || *end || value < minimum || value > maximum) return false;
    output = static_cast<uint32_t>(value);
    return true;
}

bool ParseFloat(const char* text, float minimum, float maximum, float& output) {
    if (!text || !*text) return false;
    char* end = nullptr;
    errno = 0;
    const float value = std::strtof(text, &end);
    if (errno || *end || !std::isfinite(value) || value < minimum ||
        value > maximum) return false;
    output = value;
    return true;
}

bool ParseOptions(int argc, char** argv, Options& options) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto value = [&]() -> const char* {
            return ++index < argc ? argv[index] : nullptr;
        };
        if (argument == "--help" || argument == "-h") {
            Usage();
            std::exit(0);
        } else if (argument == "--soundfont") {
            const char* path = value();
            if (!path) return false;
            try { options.soundfont = converter.from_bytes(path); }
            catch (const std::range_error&) { return false; }
        } else if (argument == "--audio-device") {
            const char* name = value(); if (!name) return false;
            options.pcmDevice = name;
        } else if (argument == "--client-name") {
            const char* name = value(); if (!name) return false;
            options.clientName = name;
        } else if (argument == "--sample-rate") {
            if (!ParseU32(value(), 8000u, 384000u, options.sampleRate)) return false;
        } else if (argument == "--buffer-frames") {
            if (!ParseU32(value(), 16u, 8192u, options.bufferFrames)) return false;
        } else if (argument == "--max-voices") {
            if (!ParseU32(value(), 1u, svms::kMaxPolyphony, options.maxVoices)) return false;
        } else if (argument == "--render-threads") {
            if (!ParseU32(value(), 0u, 64u, options.renderThreads)) return false;
        } else if (argument == "--master-volume") {
            if (!ParseFloat(value(), 0.0f, 4.0f, options.masterVolume)) return false;
        } else if (argument == "--backend") {
            const char* name = value(); if (!name) return false;
            if (std::strcmp(name, "auto") == 0)
                options.backend = svms::RenderBackend::AVX512;
            else if (std::strcmp(name, "scalar") == 0)
                options.backend = svms::RenderBackend::Scalar;
            else if (std::strcmp(name, "sse2") == 0)
                options.backend = svms::RenderBackend::SSE2;
            else if (std::strcmp(name, "avx2") == 0)
                options.backend = svms::RenderBackend::AVX2;
            else return false;
        } else if (argument == "--no-limiter") {
            options.limiterEnabled = false;
        } else {
            return false;
        }
    }
    return !options.soundfont.empty();
}

struct MidiEvent {
    uint64_t timestampNs;
    uint64_t sequence;
    uint64_t targetFrame;
    uint32_t message;
};

uint32_t PackMessage(uint8_t status, uint8_t first, uint8_t second) {
    return uint32_t(status) | (uint32_t(first) << 8u) |
           (uint32_t(second) << 16u);
}

bool ConvertEvent(const snd_seq_event_t& input, MidiEvent& output) {
    uint8_t status = 0, first = 0, second = 0;
    switch (input.type) {
    case SND_SEQ_EVENT_NOTEON:
        status = uint8_t(0x90u | (input.data.note.channel & 15u));
        first = input.data.note.note;
        second = input.data.note.velocity;
        break;
    case SND_SEQ_EVENT_NOTEOFF:
        status = uint8_t(0x80u | (input.data.note.channel & 15u));
        first = input.data.note.note;
        second = input.data.note.velocity;
        break;
    case SND_SEQ_EVENT_CONTROLLER:
        status = uint8_t(0xb0u | (input.data.control.channel & 15u));
        first = uint8_t((std::max)(
            0, (std::min)(127, int(input.data.control.param))));
        second = uint8_t((std::max)(0, (std::min)(127, input.data.control.value)));
        break;
    case SND_SEQ_EVENT_PGMCHANGE:
        status = uint8_t(0xc0u | (input.data.control.channel & 15u));
        first = uint8_t((std::max)(0, (std::min)(127, input.data.control.value)));
        break;
    case SND_SEQ_EVENT_PITCHBEND: {
        status = uint8_t(0xe0u | (input.data.control.channel & 15u));
        const int bend = (std::max)(0, (std::min)(16383,
                                                 input.data.control.value + 8192));
        first = uint8_t(bend & 127);
        second = uint8_t((bend >> 7) & 127);
        break;
    }
    default:
        return false;
    }
    output.message = PackMessage(status, first, second);
    return true;
}

class AlsaSequencerInput {
public:
    ~AlsaSequencerInput() { Close(); }

    bool Open(const std::string& clientName, std::string& error) {
        int result = snd_seq_open(&sequence_, "default", SND_SEQ_OPEN_INPUT,
                                  SND_SEQ_NONBLOCK);
        if (result < 0) {
            error = std::string("cannot open ALSA Sequencer: ") +
                    snd_strerror(result);
            return false;
        }
        snd_seq_set_client_name(sequence_, clientName.c_str());
        port_ = snd_seq_create_simple_port(
            sequence_, "MIDI In",
            SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_SYNTHESIZER |
                SND_SEQ_PORT_TYPE_APPLICATION);
        if (port_ < 0) {
            error = std::string("cannot create ALSA MIDI port: ") +
                    snd_strerror(port_);
            Close();
            return false;
        }
        return true;
    }

    void Close() {
        if (sequence_) snd_seq_close(sequence_);
        sequence_ = nullptr;
        port_ = -1;
    }

    int Client() const { return sequence_ ? snd_seq_client_id(sequence_) : -1; }
    int Port() const { return port_; }

    template <typename Queue>
    void Run(Queue& queue, std::atomic<uint64_t>& sequenceNumber,
             std::atomic<uint64_t>& received) {
        while (!g_stop) {
            snd_seq_event_t* event = nullptr;
            const int result = snd_seq_event_input(sequence_, &event);
            if (result == -EAGAIN) {
                std::this_thread::sleep_for(std::chrono::microseconds(250));
                continue;
            }
            if (result < 0) {
                if (result != -EINTR)
                    std::fprintf(stderr, "ALSA MIDI read failed: %s\n",
                                 snd_strerror(result));
                if (!g_stop)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            MidiEvent converted{};
            converted.timestampNs = MonotonicNanoseconds();
            converted.sequence = sequenceNumber.fetch_add(
                1u, std::memory_order_relaxed);
            if (event && ConvertEvent(*event, converted)) {
                while (!g_stop && !queue.TryPush(converted)) sched_yield();
                received.fetch_add(1u, std::memory_order_relaxed);
            }
            if (event) snd_seq_free_event(event);
        }
    }

private:
    snd_seq_t* sequence_ = nullptr;
    int port_ = -1;
};

class AlsaPcmOutput {
public:
    ~AlsaPcmOutput() { Close(); }

    bool Open(const Options& options, std::string& error) {
        int result = snd_pcm_open(&pcm_, options.pcmDevice.c_str(),
                                  SND_PCM_STREAM_PLAYBACK, 0);
        if (result < 0) {
            error = std::string("cannot open ALSA PCM '") + options.pcmDevice +
                    "': " + snd_strerror(result);
            return false;
        }
        const uint32_t latencyUs = uint32_t(
            (uint64_t(options.bufferFrames) * 4u * 1000000u) /
            options.sampleRate);
        result = snd_pcm_set_params(pcm_, SND_PCM_FORMAT_FLOAT_LE,
                                    SND_PCM_ACCESS_RW_INTERLEAVED, 2,
                                    options.sampleRate, 1, latencyUs);
        if (result < 0) {
            error = std::string("cannot configure ALSA PCM: ") +
                    snd_strerror(result);
            Close();
            return false;
        }
        snd_pcm_uframes_t buffer = 0, period = 0;
        if (snd_pcm_get_params(pcm_, &buffer, &period) == 0) {
            bufferFrames_ = uint32_t((std::min<snd_pcm_uframes_t>)(
                buffer, UINT32_MAX));
            periodFrames_ = uint32_t((std::min<snd_pcm_uframes_t>)(
                period, UINT32_MAX));
        }
        return true;
    }

    void Close() {
        if (pcm_) {
            snd_pcm_drop(pcm_);
            snd_pcm_close(pcm_);
        }
        pcm_ = nullptr;
    }

    bool Write(const float* interleaved, uint32_t frameCount) {
        uint32_t written = 0;
        while (written < frameCount && !g_stop) {
            snd_pcm_sframes_t result = snd_pcm_writei(
                pcm_, interleaved + size_t(written) * 2u, frameCount - written);
            if (result == -EINTR) continue;
            if (result < 0) {
                result = snd_pcm_recover(pcm_, int(result), 1);
                if (result < 0) {
                    std::fprintf(stderr, "ALSA PCM write failed: %s\n",
                                 snd_strerror(int(result)));
                    return false;
                }
                continue;
            }
            written += uint32_t(result);
        }
        return written == frameCount;
    }

    uint32_t BufferFrames() const { return bufferFrames_; }
    uint32_t PeriodFrames() const { return periodFrames_; }

private:
    snd_pcm_t* pcm_ = nullptr;
    uint32_t bufferFrames_ = 0;
    uint32_t periodFrames_ = 0;
};

uint64_t TimestampToFrame(uint64_t timestamp, uint64_t epoch,
                          uint32_t sampleRate, uint64_t leadFrames) {
    if (timestamp <= epoch) return leadFrames;
    const unsigned __int128 scaled =
        static_cast<unsigned __int128>(timestamp - epoch) * sampleRate;
    return leadFrames + uint64_t(scaled / 1000000000ull);
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        Usage();
        return 2;
    }

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    svms::StandaloneSynthConfig synthConfig{};
    synthConfig.soundfont = options.soundfont;
    synthConfig.sampleRate = options.sampleRate;
    synthConfig.maxVoices = options.maxVoices;
    synthConfig.renderThreads = options.renderThreads;
    synthConfig.maxBlockFrames = options.bufferFrames;
    synthConfig.masterVolume = options.masterVolume;
    synthConfig.limiterEnabled = options.limiterEnabled;
    synthConfig.backend = options.backend;

    svms::StandaloneSynth synth;
    std::string error;
    if (!synth.Initialize(synthConfig, error)) {
        std::fprintf(stderr, "svmsd: %s\n", error.c_str());
        return 1;
    }

    AlsaPcmOutput pcm;
    if (!pcm.Open(options, error)) {
        std::fprintf(stderr, "svmsd: %s\n", error.c_str());
        return 1;
    }
    AlsaSequencerInput midi;
    if (!midi.Open(options.clientName, error)) {
        std::fprintf(stderr, "svmsd: %s\n", error.c_str());
        return 1;
    }

    svms::MPSCQueue<MidiEvent, kMidiQueueCapacity> queue;
    std::atomic<uint64_t> ingressSequence{0u};
    std::atomic<uint64_t> received{0u};
    std::thread midiThread([&] {
        midi.Run(queue, ingressSequence, received);
    });

    const uint64_t epochNs = MonotonicNanoseconds();
    const uint64_t leadFrames = uint64_t(pcm.BufferFrames()) +
                                pcm.PeriodFrames() + options.bufferFrames;
    std::fprintf(stderr,
                 "SVMS V3 ready: ALSA MIDI %d:%d -> PCM '%s', %u Hz, "
                 "%u voices, %s, lead %llu frames. Press Ctrl+C to stop.\n",
                 midi.Client(), midi.Port(), options.pcmDevice.c_str(),
                 options.sampleRate, options.maxVoices, synth.Backend(),
                 static_cast<unsigned long long>(leadFrames));

    std::vector<MidiEvent> pending;
    pending.reserve(kMidiQueueCapacity);
    std::vector<float> left(options.bufferFrames);
    std::vector<float> right(options.bufferFrames);
    std::vector<float> interleaved(size_t(options.bufferFrames) * 2u);
    size_t pendingHead = 0;
    uint64_t outputFrame = 0, dispatched = 0, late = 0;
    auto lastTelemetry = std::chrono::steady_clock::now();

    while (!g_stop) {
        if (pendingHead && (pendingHead == pending.size() ||
                            pendingHead >= pending.size() / 2u)) {
            pending.erase(pending.begin(), pending.begin() + pendingHead);
            pendingHead = 0;
        }
        MidiEvent event{};
        while (pending.size() - pendingHead < kMidiQueueCapacity &&
               queue.TryPop(event)) {
            event.targetFrame = TimestampToFrame(event.timestampNs, epochNs,
                                                 options.sampleRate, leadFrames);
            pending.push_back(event);
        }
        if (pending.size() - pendingHead > 1u) {
            std::sort(pending.begin() + pendingHead, pending.end(),
                      [](const MidiEvent& leftEvent, const MidiEvent& rightEvent) {
                if (leftEvent.targetFrame != rightEvent.targetFrame)
                    return leftEvent.targetFrame < rightEvent.targetFrame;
                return leftEvent.sequence < rightEvent.sequence;
            });
        }

        const uint64_t blockEnd = outputFrame + options.bufferFrames;
        uint32_t cursor = 0;
        while (pendingHead < pending.size() &&
               pending[pendingHead].targetFrame < blockEnd) {
            MidiEvent& due = pending[pendingHead];
            uint64_t target = due.targetFrame;
            if (target < outputFrame + cursor) {
                target = outputFrame + cursor;
                ++late;
            }
            const uint32_t span = uint32_t(target - (outputFrame + cursor));
            if (span) {
                synth.Render(left.data() + cursor, right.data() + cursor,
                             span, outputFrame + cursor);
                cursor += span;
            }
            synth.Dispatch(due.message, target);
            ++dispatched;
            ++pendingHead;
        }
        if (cursor < options.bufferFrames) {
            synth.Render(left.data() + cursor, right.data() + cursor,
                         options.bufferFrames - cursor, outputFrame + cursor);
        }
        for (uint32_t frame = 0; frame < options.bufferFrames; ++frame) {
            interleaved[size_t(frame) * 2u] = left[frame];
            interleaved[size_t(frame) * 2u + 1u] = right[frame];
        }
        if (!pcm.Write(interleaved.data(), options.bufferFrames)) {
            g_stop = 1;
            break;
        }
        outputFrame = blockEnd;

        const auto now = std::chrono::steady_clock::now();
        if (now - lastTelemetry >= std::chrono::seconds(1)) {
            std::fprintf(stderr,
                         "\rvoices %u/%u free %u steals %u | events %llu "
                         "queued %u pending %zu late %llu   ",
                         synth.Active(), options.maxVoices, synth.Free(),
                         synth.Steals(),
                         static_cast<unsigned long long>(dispatched),
                         queue.Size(), pending.size() - pendingHead,
                         static_cast<unsigned long long>(late));
            std::fflush(stderr);
            lastTelemetry = now;
        }
    }

    g_stop = 1;
    midiThread.join();
    std::fputs("\nSVMS V3 stopped.\n", stderr);
    return 0;
}
