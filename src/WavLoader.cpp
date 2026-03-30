#include "WavLoader.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#define DR_FLAC_IMPLEMENTATION
#include "../third_party_dr_flac.h"

namespace {

static std::string ToLower(const std::string &value) {
  std::string lowered = value;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char ch) { return (char)tolower(ch); });
  return lowered;
}

static bool ReadExactly(std::ifstream &file, void *buffer, size_t bytes) {
  file.read(reinterpret_cast<char *>(buffer), static_cast<std::streamsize>(bytes));
  return file.good();
}

static uint16_t ReadLe16(const unsigned char *data) {
  return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

static uint32_t ReadLe32(const unsigned char *data) {
  return static_cast<uint32_t>(data[0] | (data[1] << 8) | (data[2] << 16) |
                               (data[3] << 24));
}

static int32_t SignExtend24(uint32_t value) {
  if (value & 0x800000u)
    value |= 0xFF000000u;
  return static_cast<int32_t>(value);
}

static bool IsAbsolutePath(const std::string &path) {
  return path.size() > 2 && path[1] == ':' &&
         (path[2] == '\\' || path[2] == '/');
}

static void SetError(std::string *errorMessage, const std::string &text) {
  if (errorMessage)
    *errorMessage = text;
}

} // namespace

bool WavLoader::LoadWav(const std::string &filename, AudioSample &sample,
                        std::string *errorMessage) {
  sample = AudioSample();

  std::ifstream file(filename.c_str(), std::ios::binary);
  if (!file.is_open()) {
    SetError(errorMessage, "Could not open WAV file");
    return false;
  }

  unsigned char riffHeader[12];
  if (!ReadExactly(file, riffHeader, sizeof(riffHeader))) {
    SetError(errorMessage, "WAV header is truncated");
    return false;
  }

  if (memcmp(riffHeader, "RIFF", 4) != 0 || memcmp(riffHeader + 8, "WAVE", 4) != 0) {
    SetError(errorMessage, "File is not a RIFF/WAVE file");
    return false;
  }

  uint16_t formatTag = 0;
  uint16_t numChannels = 0;
  uint32_t sampleRate = 0;
  uint16_t bitsPerSample = 0;
  std::vector<unsigned char> rawPcm;
  bool foundFmt = false;
  bool foundData = false;

  while (file.good() && !file.eof()) {
    unsigned char chunkHeader[8];
    if (!ReadExactly(file, chunkHeader, sizeof(chunkHeader)))
      break;

    uint32_t chunkSize = ReadLe32(chunkHeader + 4);
    std::vector<unsigned char> chunkData(chunkSize);
    if (chunkSize > 0) {
      if (!ReadExactly(file, chunkData.data(), chunkSize)) {
        SetError(errorMessage, "WAV chunk is truncated");
        return false;
      }
    }
    if (chunkSize & 1)
      file.seekg(1, std::ios::cur);

    if (memcmp(chunkHeader, "fmt ", 4) == 0) {
      if (chunkSize < 16) {
        SetError(errorMessage, "WAV fmt chunk is too small");
        return false;
      }
      formatTag = ReadLe16(chunkData.data() + 0);
      numChannels = ReadLe16(chunkData.data() + 2);
      sampleRate = ReadLe32(chunkData.data() + 4);
      bitsPerSample = ReadLe16(chunkData.data() + 14);
      foundFmt = true;
    } else if (memcmp(chunkHeader, "data", 4) == 0) {
      rawPcm.swap(chunkData);
      foundData = true;
    }
  }

  if (!foundFmt || !foundData) {
    SetError(errorMessage, "WAV is missing fmt or data chunk");
    return false;
  }

  if (numChannels == 0 || numChannels > 2) {
    SetError(errorMessage, "Only mono and stereo WAV files are supported");
    return false;
  }

  const int bytesPerSample = bitsPerSample / 8;
  if (bytesPerSample <= 0) {
    SetError(errorMessage, "Unsupported WAV bit depth");
    return false;
  }
  const int blockAlign = numChannels * bytesPerSample;
  if (blockAlign <= 0 || rawPcm.size() % static_cast<size_t>(blockAlign) != 0) {
    SetError(errorMessage, "WAV data size does not match the format");
    return false;
  }

  const int frameCount = static_cast<int>(rawPcm.size() / blockAlign);
  sample.data.resize(static_cast<size_t>(frameCount) * numChannels);
  sample.channels = numChannels;
  sample.sampleRate = static_cast<int>(sampleRate);
  sample.frameCount = frameCount;

  for (int frame = 0; frame < frameCount; ++frame) {
    for (int channel = 0; channel < numChannels; ++channel) {
      const unsigned char *src =
          &rawPcm[(static_cast<size_t>(frame) * numChannels + channel) * bytesPerSample];
      float value = 0.0f;
      if (formatTag == 1) {
        switch (bitsPerSample) {
        case 8:
          value = (static_cast<int>(src[0]) - 128) / 128.0f;
          break;
        case 16: {
          int16_t v = static_cast<int16_t>(ReadLe16(src));
          value = v / 32768.0f;
          break;
        }
        case 24: {
          uint32_t v = src[0] | (src[1] << 8) | (src[2] << 16);
          value = SignExtend24(v) / 8388608.0f;
          break;
        }
        case 32: {
          int32_t v = static_cast<int32_t>(ReadLe32(src));
          value = v / 2147483648.0f;
          break;
        }
        default:
          SetError(errorMessage, "Unsupported PCM WAV bit depth");
          return false;
        }
      } else if (formatTag == 3 && bitsPerSample == 32) {
        float floatValue = 0.0f;
        memcpy(&floatValue, src, sizeof(float));
        value = floatValue;
      } else {
        SetError(errorMessage, "Only PCM and 32-bit float WAV files are supported");
        return false;
      }

      sample.data[static_cast<size_t>(frame) * numChannels + channel] = value;
    }
  }

  return true;
}

bool WavLoader::LoadFlac(const std::string &filename, AudioSample &sample,
                         std::string *errorMessage) {
  sample = AudioSample();

  unsigned int channels = 0;
  unsigned int sampleRate = 0;
  drflac_uint64 totalFrameCount = 0;
  float *decoded = drflac_open_file_and_read_pcm_frames_f32(
      filename.c_str(), &channels, &sampleRate, &totalFrameCount, NULL);
  if (!decoded) {
    SetError(errorMessage, "Could not decode FLAC file");
    return false;
  }

  if (channels == 0 || channels > 2) {
    drflac_free(decoded, NULL);
    SetError(errorMessage, "Only mono and stereo FLAC files are supported");
    return false;
  }

  sample.channels = static_cast<int>(channels);
  sample.sampleRate = static_cast<int>(sampleRate);
  sample.frameCount = static_cast<int>(totalFrameCount);
  sample.data.assign(decoded, decoded + totalFrameCount * channels);
  drflac_free(decoded, NULL);
  return true;
}

bool WavLoader::LoadAudioFile(const std::string &filename, AudioSample &sample,
                              std::string *errorMessage) {
  size_t dot = filename.find_last_of('.');
  std::string extension =
      dot == std::string::npos ? std::string() : ToLower(filename.substr(dot + 1));
  if (extension == "wav")
    return LoadWav(filename, sample, errorMessage);
  if (extension == "flac")
    return LoadFlac(filename, sample, errorMessage);

  SetError(errorMessage, "Unsupported sample file extension");
  return false;
}
