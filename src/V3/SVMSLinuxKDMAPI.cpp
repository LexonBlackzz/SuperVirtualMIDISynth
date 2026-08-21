#include "SVMSMPSCQueue.h"
#include "SVMSStandaloneSynth.h"

#include <alsa/asoundlib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <codecvt>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <locale>
#include <mutex>
#include <new>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {

using json = nlohmann::json;

constexpr uint32_t kDefaultEventCapacity = 393216u;

uint64_t MonotonicNanoseconds() {
    timespec value{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &value);
    return uint64_t(value.tv_sec) * 1000000000ull + uint64_t(value.tv_nsec);
}

bool FileExists(const std::string& path) {
    struct stat status{};
    return !path.empty() && stat(path.c_str(), &status) == 0 &&
           S_ISREG(status.st_mode);
}

bool IsAbsolute(const std::string& path) {
    return !path.empty() && path[0] == '/';
}

std::string DirectoryName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0u) return "/";
    return path.substr(0u, slash);
}

std::string JoinPath(const std::string& directory, const std::string& name) {
    if (name.empty() || IsAbsolute(name)) return name;
    if (directory.empty() || directory == ".") return name;
    return directory.back() == '/' ? directory + name
                                   : directory + "/" + name;
}

std::string CurrentDirectory() {
    std::vector<char> buffer(4096u);
    return getcwd(buffer.data(), buffer.size()) ? buffer.data() : ".";
}

std::string LibraryDirectory() {
    Dl_info information{};
    if (dladdr(reinterpret_cast<void*>(&LibraryDirectory), &information) &&
        information.dli_fname) {
        char resolved[PATH_MAX]{};
        if (realpath(information.dli_fname, resolved))
            return DirectoryName(resolved);
        return DirectoryName(information.dli_fname);
    }
    return CurrentDirectory();
}

std::wstring Utf8ToWide(const std::string& value) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    return converter.from_bytes(value);
}

struct RuntimeOptions {
    svms::StandaloneSynthConfig synth;
    std::string pcmDevice = "default";
    uint32_t bufferFrames = 2048u;
    uint32_t eventCapacity = kDefaultEventCapacity;
    std::string configPath;
};

template <typename T>
void ReadNumber(const json& object, const char* key, T minimum, T maximum,
                T& destination) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number()) return;
    try {
        const T value = found->get<T>();
        if (value >= minimum && value <= maximum) destination = value;
    } catch (...) {}
}

void ReadBoolean(const json& object, const char* key, bool& destination) {
    const auto found = object.find(key);
    if (found != object.end() && found->is_boolean())
        destination = found->get<bool>();
}

std::string LocateConfig(const std::string& libraryDirectory) {
    if (const char* explicitPath = std::getenv("SVMS_CONFIG")) {
        if (FileExists(explicitPath)) return explicitPath;
    }
    const std::string local = JoinPath(CurrentDirectory(), "config.json");
    if (FileExists(local)) return local;
    const std::string besideLibrary = JoinPath(libraryDirectory, "config.json");
    if (FileExists(besideLibrary)) return besideLibrary;
    std::string configHome;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) configHome = xdg;
    else if (const char* home = std::getenv("HOME"))
        configHome = JoinPath(home, ".config");
    const std::string roamingEquivalent = JoinPath(
        configHome, "SuperVirtualMIDISynth/config.json");
    return FileExists(roamingEquivalent) ? roamingEquivalent : std::string{};
}

bool LoadOptions(RuntimeOptions& options, std::string& error) {
    options.synth.sampleRate = 44100u;
    options.synth.maxVoices = 4096u;
    options.synth.renderThreads = 0u;
    options.synth.maxBlockFrames = options.bufferFrames;
    options.synth.masterVolume = 1.0f;
    options.synth.limiterEnabled = true;
    options.synth.limiterAlgorithm = svms::LimiterAlgorithm::Classic;
    options.synth.limiterThreshold = 0.95f;
    options.synth.limiterLookaheadMs = 3.0f;
    options.synth.limiterAttackMs = 0.5f;
    options.synth.limiterReleaseMs = 100.0f;

    const std::string libraryDirectory = LibraryDirectory();
    options.configPath = LocateConfig(libraryDirectory);
    std::string soundfont = "gm.sf2";
    if (!options.configPath.empty()) {
        try {
            std::ifstream stream(options.configPath);
            json root;
            stream >> root;
            if (root.is_object()) {
                if (auto audio = root.find("audio");
                    audio != root.end() && audio->is_object()) {
                    ReadNumber(*audio, "sample_rate", 8000u, 384000u,
                               options.synth.sampleRate);
                    ReadNumber(*audio, "buffer_frames", 16u, 8192u,
                               options.bufferFrames);
                    const auto linuxDevice = audio->find("linux_device");
                    if (linuxDevice != audio->end() && linuxDevice->is_string())
                        options.pcmDevice = linuxDevice->get<std::string>();
                }
                if (auto synth = root.find("synth");
                    synth != root.end() && synth->is_object()) {
                    ReadNumber(*synth, "max_voices", 1u, svms::kMaxPolyphony,
                               options.synth.maxVoices);
                    ReadNumber(*synth, "render_threads", 0u, 64u,
                               options.synth.renderThreads);
                    ReadNumber(*synth, "master_volume", 0.0f, 4.0f,
                               options.synth.masterVolume);
                    const auto selected = synth->find("soundfont");
                    if (selected != synth->end() && selected->is_string())
                        soundfont = selected->get<std::string>();
                }
                if (auto events = root.find("events");
                    events != root.end() && events->is_object()) {
                    ReadNumber(*events, "ring_capacity", 4096u,
                               svms::kMaxConfigurableEventCapacity,
                               options.eventCapacity);
                }
                if (auto limiter = root.find("limiter");
                    limiter != root.end() && limiter->is_object()) {
                    ReadBoolean(*limiter, "enabled",
                                options.synth.limiterEnabled);
                    ReadNumber(*limiter, "threshold", 0.1f, 1.0f,
                               options.synth.limiterThreshold);
                    ReadNumber(*limiter, "lookahead_ms", 0.0f, 20.0f,
                               options.synth.limiterLookaheadMs);
                    ReadNumber(*limiter, "attack_ms", 0.01f, 100.0f,
                               options.synth.limiterAttackMs);
                    ReadNumber(*limiter, "release_ms", 1.0f, 5000.0f,
                               options.synth.limiterReleaseMs);
                    const auto algorithm = limiter->find("algorithm");
                    if (algorithm != limiter->end() && algorithm->is_string() &&
                        algorithm->get<std::string>() == "adaptive")
                        options.synth.limiterAlgorithm =
                            svms::LimiterAlgorithm::Adaptive;
                }
            }
        } catch (const std::exception& exception) {
            error = std::string("cannot parse config.json: ") + exception.what();
            return false;
        }
    }

    if (const char* environmentSoundfont = std::getenv("SVMS_SOUNDFONT"))
        soundfont = environmentSoundfont;
    if (const char* environmentDevice = std::getenv("SVMS_AUDIO_DEVICE"))
        options.pcmDevice = environmentDevice;

    const std::string configDirectory = options.configPath.empty()
        ? CurrentDirectory() : DirectoryName(options.configPath);
    std::string resolvedSoundfont = IsAbsolute(soundfont)
        ? soundfont : JoinPath(configDirectory, soundfont);
    if (!FileExists(resolvedSoundfont)) {
        const std::string besideLibrary = JoinPath(libraryDirectory, soundfont);
        if (FileExists(besideLibrary)) resolvedSoundfont = besideLibrary;
    }
    if (!FileExists(resolvedSoundfont)) {
        error = "SoundFont not found: " + resolvedSoundfont +
                " (set synth.soundfont or SVMS_SOUNDFONT)";
        return false;
    }
    try {
        options.synth.soundfont = Utf8ToWide(resolvedSoundfont);
    } catch (const std::range_error&) {
        error = "SoundFont path is not valid UTF-8";
        return false;
    }
    options.synth.maxBlockFrames = options.bufferFrames;
    return true;
}

class PcmOutput {
public:
    ~PcmOutput() { Close(); }

    bool Open(const RuntimeOptions& options, std::string& error) {
        int result = snd_pcm_open(&pcm_, options.pcmDevice.c_str(),
                                  SND_PCM_STREAM_PLAYBACK, 0);
        if (result < 0) {
            error = std::string("cannot open ALSA PCM '") + options.pcmDevice +
                    "': " + snd_strerror(result);
            return false;
        }
        const uint32_t latencyUs = uint32_t(
            (uint64_t(options.bufferFrames) * 4u * 1000000u) /
            options.synth.sampleRate);
        result = snd_pcm_set_params(pcm_, SND_PCM_FORMAT_FLOAT_LE,
                                    SND_PCM_ACCESS_RW_INTERLEAVED, 2,
                                    options.synth.sampleRate, 1, latencyUs);
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

    bool Write(const float* samples, uint32_t frames,
               const std::atomic<bool>& running) {
        uint32_t written = 0u;
        while (written < frames && running.load(std::memory_order_acquire)) {
            snd_pcm_sframes_t result = snd_pcm_writei(
                pcm_, samples + size_t(written) * 2u, frames - written);
            if (result == -EINTR) continue;
            if (result < 0) {
                result = snd_pcm_recover(pcm_, int(result), 1);
                if (result < 0) return false;
                continue;
            }
            written += uint32_t(result);
        }
        return written == frames;
    }

    uint64_t LeadFrames(uint32_t quantum) const {
        return uint64_t(bufferFrames_) + periodFrames_ + quantum;
    }

private:
    snd_pcm_t* pcm_ = nullptr;
    uint32_t bufferFrames_ = 0u, periodFrames_ = 0u;
};

enum class EventKind : uint32_t { Midi, Reset };

struct QueuedEvent {
    uint64_t timestampNs = 0u;
    uint64_t sequence = 0u;
    uint64_t targetFrame = 0u;
    uint32_t message = 0u;
    EventKind kind = EventKind::Midi;
};

uint64_t TimestampToFrame(uint64_t timestamp, uint64_t epoch,
                          uint32_t sampleRate, uint64_t leadFrames) {
    if (timestamp <= epoch) return leadFrames;
    const unsigned __int128 scaled =
        static_cast<unsigned __int128>(timestamp - epoch) * sampleRate;
    return leadFrames + uint64_t(scaled / 1000000000ull);
}

class KdmapiRuntime {
public:
    ~KdmapiRuntime() { Shutdown(); }

    bool Initialize() {
        std::lock_guard<std::mutex> guard(controlMutex_);
        if (running_.load(std::memory_order_acquire)) return true;
        if (audioThread_.joinable()) audioThread_.join();
        pcm_.reset();
        synth_.reset();
        RuntimeOptions options;
        std::string error;
        if (!LoadOptions(options, error)) {
            std::fprintf(stderr, "[SVMS Linux] %s\n", error.c_str());
            return false;
        }
        std::unique_ptr<svms::StandaloneSynth> synth(
            new (std::nothrow) svms::StandaloneSynth());
        if (!synth || !synth->Initialize(options.synth, error)) {
            std::fprintf(stderr, "[SVMS Linux] synth initialization failed: %s\n",
                         error.c_str());
            return false;
        }
        std::unique_ptr<PcmOutput> pcm(new (std::nothrow) PcmOutput());
        if (!pcm || !pcm->Open(options, error)) {
            std::fprintf(stderr, "[SVMS Linux] audio initialization failed: %s\n",
                         error.c_str());
            return false;
        }
        if (!queue_.ConfigureCapacity(options.eventCapacity)) {
            std::fprintf(stderr, "[SVMS Linux] cannot allocate MIDI queue\n");
            return false;
        }
        options_ = options;
        synth_ = std::move(synth);
        pcm_ = std::move(pcm);
        epochNs_ = MonotonicNanoseconds();
        leadFrames_ = pcm_->LeadFrames(options_.bufferFrames);
        ingressSequence_.store(0u, std::memory_order_relaxed);
        renderingTimeBits_.store(0u, std::memory_order_relaxed);
        activeVoices_.store(0u, std::memory_order_relaxed);
        freeVoices_.store(options_.synth.maxVoices, std::memory_order_relaxed);
        voiceSteals_.store(0u, std::memory_order_relaxed);
        running_.store(true, std::memory_order_release);
        try {
            audioThread_ = std::thread(&KdmapiRuntime::AudioThread, this);
        } catch (...) {
            running_.store(false, std::memory_order_release);
            pcm_.reset();
            synth_.reset();
            return false;
        }
        std::fprintf(stderr,
            "[SVMS Linux] KDMAPI ready: SF2 '%ls', PCM '%s', %u Hz, "
            "%u frames, %u voices, %s, queue %u\n",
            options_.synth.soundfont.c_str(), options_.pcmDevice.c_str(),
            options_.synth.sampleRate, options_.bufferFrames,
            options_.synth.maxVoices, synth_->Backend(),
            options_.eventCapacity);
        return true;
    }

    bool Submit(uint32_t message, EventKind kind = EventKind::Midi) {
        if (!running_.load(std::memory_order_acquire)) return false;
        QueuedEvent event{};
        event.timestampNs = MonotonicNanoseconds();
        event.sequence = ingressSequence_.fetch_add(
            1u, std::memory_order_relaxed);
        event.message = message;
        event.kind = kind;
        while (running_.load(std::memory_order_acquire)) {
            if (queue_.TryPush(event)) return true;
            sched_yield();
        }
        return false;
    }

    void Shutdown() {
        std::lock_guard<std::mutex> guard(controlMutex_);
        running_.store(false, std::memory_order_release);
        if (audioThread_.joinable()) audioThread_.join();
        pcm_.reset();
        synth_.reset();
        activeVoices_.store(0u, std::memory_order_relaxed);
        freeVoices_.store(0u, std::memory_order_relaxed);
    }

    svms::SnappyVoiceStatistics Statistics() const {
        svms::SnappyVoiceStatistics result{};
        result.activeVoices = activeVoices_.load(std::memory_order_relaxed);
        result.freeVoices = freeVoices_.load(std::memory_order_relaxed);
        result.voiceSteals = voiceSteals_.load(std::memory_order_relaxed);
        return result;
    }

    float RenderingTime() const {
        const uint32_t bits = renderingTimeBits_.load(std::memory_order_relaxed);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

private:
    void PublishStatistics(float milliseconds) {
        activeVoices_.store(synth_->Active(), std::memory_order_relaxed);
        freeVoices_.store(synth_->Free(), std::memory_order_relaxed);
        voiceSteals_.store(synth_->Steals(), std::memory_order_relaxed);
        uint32_t bits = 0u;
        std::memcpy(&bits, &milliseconds, sizeof(bits));
        renderingTimeBits_.store(bits, std::memory_order_relaxed);
    }

    void AudioThread() {
        std::vector<QueuedEvent> pending;
        pending.reserve(options_.eventCapacity);
        std::vector<float> left(options_.bufferFrames);
        std::vector<float> right(options_.bufferFrames);
        std::vector<float> interleaved(size_t(options_.bufferFrames) * 2u);
        size_t pendingHead = 0u;
        uint64_t outputFrame = 0u;
        while (running_.load(std::memory_order_acquire)) {
            const auto renderStart = std::chrono::steady_clock::now();
            if (pendingHead && (pendingHead == pending.size() ||
                                pendingHead >= pending.size() / 2u)) {
                pending.erase(pending.begin(), pending.begin() + pendingHead);
                pendingHead = 0u;
            }
            QueuedEvent event{};
            while (pending.size() - pendingHead < options_.eventCapacity &&
                   queue_.TryPop(event)) {
                event.targetFrame = TimestampToFrame(
                    event.timestampNs, epochNs_, options_.synth.sampleRate,
                    leadFrames_);
                pending.push_back(event);
            }
            if (pending.size() - pendingHead > 1u) {
                std::sort(pending.begin() + pendingHead, pending.end(),
                    [](const QueuedEvent& leftEvent,
                       const QueuedEvent& rightEvent) {
                        if (leftEvent.targetFrame != rightEvent.targetFrame)
                            return leftEvent.targetFrame < rightEvent.targetFrame;
                        return leftEvent.sequence < rightEvent.sequence;
                    });
            }
            const uint64_t blockEnd = outputFrame + options_.bufferFrames;
            uint32_t cursor = 0u;
            while (pendingHead < pending.size() &&
                   pending[pendingHead].targetFrame < blockEnd) {
                const QueuedEvent& due = pending[pendingHead++];
                const uint64_t writableFrame = outputFrame + cursor;
                const uint64_t target = (std::max)(due.targetFrame,
                                                   writableFrame);
                const uint32_t span = uint32_t(target - writableFrame);
                if (span) {
                    synth_->Render(left.data() + cursor, right.data() + cursor,
                                   span, writableFrame);
                    cursor += span;
                }
                if (due.kind == EventKind::Reset)
                    synth_->ResetAll(target);
                else
                    synth_->Dispatch(due.message, target);
            }
            if (cursor < options_.bufferFrames) {
                synth_->Render(left.data() + cursor, right.data() + cursor,
                               options_.bufferFrames - cursor,
                               outputFrame + cursor);
            }
            for (uint32_t frame = 0u; frame < options_.bufferFrames; ++frame) {
                interleaved[size_t(frame) * 2u] = left[frame];
                interleaved[size_t(frame) * 2u + 1u] = right[frame];
            }
            const auto renderEnd = std::chrono::steady_clock::now();
            const float milliseconds = float(
                std::chrono::duration<double, std::milli>(
                    renderEnd - renderStart).count());
            PublishStatistics(milliseconds);
            if (!pcm_->Write(interleaved.data(), options_.bufferFrames,
                             running_)) {
                std::fprintf(stderr, "[SVMS Linux] ALSA output stopped\n");
                running_.store(false, std::memory_order_release);
                break;
            }
            outputFrame = blockEnd;
        }
    }

    mutable std::mutex controlMutex_;
    svms::DynamicMPSCQueue<QueuedEvent> queue_;
    RuntimeOptions options_{};
    std::unique_ptr<svms::StandaloneSynth> synth_;
    std::unique_ptr<PcmOutput> pcm_;
    std::thread audioThread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> ingressSequence_{0u};
    std::atomic<uint32_t> renderingTimeBits_{0u};
    std::atomic<uint32_t> activeVoices_{0u};
    std::atomic<uint32_t> freeVoices_{0u};
    std::atomic<uint32_t> voiceSteals_{0u};
    uint64_t epochNs_ = 0u;
    uint64_t leadFrames_ = 0u;
};

KdmapiRuntime& Runtime() {
    static KdmapiRuntime runtime;
    return runtime;
}

} // namespace

#define SVMS_LINUX_EXPORT extern "C" __attribute__((visibility("default")))

SVMS_LINUX_EXPORT int IsKDMAPIAvailable() { return 1; }

SVMS_LINUX_EXPORT void* InitializeKDMAPIStream() {
    return Runtime().Initialize() ? reinterpret_cast<void*>(1) : nullptr;
}

SVMS_LINUX_EXPORT int TerminateKDMAPIStream() {
    Runtime().Shutdown();
    return 1;
}

SVMS_LINUX_EXPORT void ResetKDMAPIStream() {
    Runtime().Submit(0u, EventKind::Reset);
}

SVMS_LINUX_EXPORT unsigned int ReturnKDMAPIVer(
        uint32_t* major, uint32_t* minor, uint32_t* build,
        uint32_t* revision) {
    if (major) *major = 4u;
    if (minor) *minor = 1u;
    if (build) *build = 0u;
    if (revision) *revision = 0u;
    return 1u;
}

SVMS_LINUX_EXPORT void SendDirectData(uint32_t message) {
    Runtime().Submit(message);
}

SVMS_LINUX_EXPORT void SendDirectDataNoBuf(uint32_t message) {
    Runtime().Submit(message);
}

SVMS_LINUX_EXPORT int SendDirectLongData(const void*, uint32_t) { return 0; }
SVMS_LINUX_EXPORT int SendDirectLongDataNoBuf(const void*, uint32_t) { return 0; }

SVMS_LINUX_EXPORT float GetRenderingTime() {
    return Runtime().RenderingTime();
}

SVMS_LINUX_EXPORT uint32_t GetVoiceCount() {
    return Runtime().Statistics().activeVoices;
}

// Ziggy's Linux loader expects this 12-byte structure by value. SysV AMD64
// returns active/free in RAX and steals in RDX, exactly as its caller reads it.
SVMS_LINUX_EXPORT svms::SnappyVoiceStatistics GetVoiceStatistics() {
    return Runtime().Statistics();
}
