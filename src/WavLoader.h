#ifndef WAVLOADER_H
#define WAVLOADER_H

#include <string>
#include <vector>

struct AudioSample {
  std::vector<float> data;
  int sampleRate;
  int channels;
  int frameCount;

  AudioSample() : sampleRate(0), channels(0), frameCount(0) {}
};

class WavLoader {
public:
  static bool LoadWav(const std::string &filename, AudioSample &sample,
                      std::string *errorMessage = 0);
  static bool LoadFlac(const std::string &filename, AudioSample &sample,
                       std::string *errorMessage = 0);
  static bool LoadAudioFile(const std::string &filename, AudioSample &sample,
                            std::string *errorMessage = 0);
};

#endif
