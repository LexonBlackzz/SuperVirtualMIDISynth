/* TinySoundFont - v0.9 - SoundFont2 synthesizer -
   https://github.com/schellingb/TinySoundFont no warranty implied; use at your
   own risk Do this: #define TSF_IMPLEMENTATION before you include this file in
   *one* C or C++ file to create the implementation.
   // i.e. it should look like this:
   #include ...
   #include ...
   #define TSF_IMPLEMENTATION
   #include "tsf.h"

   [OPTIONAL] #define TSF_NO_STDIO to remove stdio dependency
   [OPTIONAL] #define TSF_MALLOC, TSF_REALLOC, and TSF_FREE to avoid stdlib.h
   [OPTIONAL] #define TSF_MEMCPY, TSF_MEMSET to avoid string.h
   [OPTIONAL] #define TSF_POW, TSF_POWF, TSF_EXPF, TSF_LOG, TSF_TAN, TSF_LOG10,
   TSF_SQRT to avoid math.h

   NOT YET IMPLEMENTED
     - Support for ChorusEffectsSend and ReverbEffectsSend generators
     - Better low-pass filter without lowering performance too much
     - Support for modulators

   LICENSE (MIT)

   Copyright (C) 2017-2025 Bernhard Schelling
   Based on SFZero, Copyright (C) 2012 Steve Folta
   (https://github.com/stevefolta/SFZero)

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.

*/

#ifndef TSF_INCLUDE_TSF_INL
#define TSF_INCLUDE_TSF_INL

#ifdef __cplusplus
extern "C" {
#define CPP_DEFAULT0 = 0
#else
#define CPP_DEFAULT0
#endif

// define this if you want the API functions to be static
#ifdef TSF_STATIC
#define TSFDEF static
#else
#define TSFDEF extern
#endif

// The load functions will return a pointer to a struct tsf which all functions
// thereafter take as the first parameter.
// On error the tsf_load* functions will return NULL most likely due to invalid
// data (or if the file did not exist in tsf_load_filename).
typedef struct tsf tsf;

#ifndef TSF_NO_STDIO
// Directly load a SoundFont from a .sf2 file path
TSFDEF tsf *tsf_load_filename(const char *filename);
#endif

// Load a SoundFont from a block of memory
TSFDEF tsf *tsf_load_memory(const void *buffer, int size);

// Stream structure for the generic loading
struct tsf_stream {
  // Custom data given to the functions as the first parameter
  void *data;

  // Function pointer will be called to read 'size' bytes into ptr (returns
  // number of read bytes)
  int (*read)(void *data, void *ptr, unsigned int size);

  // Function pointer will be called to skip ahead over 'count' bytes (returns 1
  // on success, 0 on error)
  int (*skip)(void *data, unsigned int count);
};

// Generic SoundFont loading method using the stream structure above
TSFDEF tsf *tsf_load(struct tsf_stream *stream);

// Copy a tsf instance from an existing one, use tsf_close to close it as well.
// All copied tsf instances and their original instance are linked, and share
// the underlying soundfont. This allows loading a soundfont only once, but
// using it for multiple independent playbacks. (This function isn't thread-safe
// without locking.)
TSFDEF tsf *tsf_copy(tsf *f);

// Free the memory related to this tsf instance
TSFDEF void tsf_close(tsf *f);

// Stop all playing notes immediately and reset all channel parameters
TSFDEF void tsf_reset(tsf *f);

// Returns the preset index from a bank and preset number, or -1 if it does not
// exist in the loaded SoundFont
TSFDEF int tsf_get_presetindex(const tsf *f, int bank, int preset_number);

// Returns the number of presets in the loaded SoundFont
TSFDEF int tsf_get_presetcount(const tsf *f);

// Returns the name of a preset index >= 0 and < tsf_get_presetcount()
TSFDEF const char *tsf_get_presetname(const tsf *f, int preset_index);

// Returns the name of a preset by bank and preset number
TSFDEF const char *tsf_bank_get_presetname(const tsf *f, int bank,
                                           int preset_number);

// Supported output modes by the render methods
enum TSFOutputMode {
  // Two channels with single left/right samples one after another
  TSF_STEREO_INTERLEAVED,
  // Two channels with all samples for the left channel first then right
  TSF_STEREO_UNWEAVED,
  // A single channel (stereo instruments are mixed into center)
  TSF_MONO
};

// Thread safety:
//
// 1. Rendering / voices:
//
// Your audio output which calls the tsf_render* functions will most likely
// run on a different thread than where the playback tsf_note* functions
// are called. In which case some sort of concurrency control like a
// mutex needs to be used so they are not called at the same time.
// Alternatively, you can pre-allocate a maximum number of voices that can
// play simultaneously by calling tsf_set_max_voices after loading.
// That way memory re-allocation will not happen during tsf_note_on and
// TSF should become mostly thread safe.
// There is a theoretical chance that ending notes would negatively influence
// a voice that is rendering at the time but it is hard to say.
// Also be aware, this has not been tested much.
//
// 2. Channels:
//
// Calls to tsf_channel_set_... functions may allocate new channels
// if no channel with that number was previously used. Make sure to
// create all channels at the beginning as required if you call tsf_render*
// from a different thread.

// Setup the parameters for the voice render methods
//   outputmode: if mono or stereo and how stereo channel data is ordered
//   samplerate: the number of samples per second (output frequency)
//   global_gain_db: volume gain in decibels (>0 means higher, <0 means lower)
TSFDEF void tsf_set_output(tsf *f, enum TSFOutputMode outputmode,
                           int samplerate, float global_gain_db CPP_DEFAULT0);

// Set the global gain as a volume factor
//   global_gain: the desired volume where 1.0 is 100%
TSFDEF void tsf_set_volume(tsf *f, float global_gain);

// Set the maximum number of voices to play simultaneously
// Depending on the soundfond, one note can cause many new voices to be started,
// so don't keep this number too low or otherwise sounds may not play.
//   max_voices: maximum number to pre-allocate and set the limit to
//   (tsf_set_max_voices returns 0 if allocation failed, otherwise 1)
TSFDEF int tsf_set_max_voices(tsf *f, int max_voices);

// Start playing a note
//   preset_index: preset index >= 0 and < tsf_get_presetcount()
//   key: note value between 0 and 127 (60 being middle C)
//   vel: velocity as a float between 0.0 (equal to note off) and 1.0 (full)
//   bank: instrument bank number (alternative to preset_index)
//   preset_number: preset number (alternative to preset_index)
//   (tsf_note_on returns 0 if the allocation of a new voice failed, otherwise
//   1) (tsf_bank_note_on returns 0 if preset does not exist or allocation
//   failed, otherwise 1)
TSFDEF int tsf_note_on(tsf *f, int preset_index, int key, float vel);
TSFDEF int tsf_note_on_ex(tsf *f, int preset_index, int key, float gain_vel,
                          int midi_velocity);
TSFDEF int tsf_bank_note_on(tsf *f, int bank, int preset_number, int key,
                            float vel);

// Stop playing a note
//   (bank_note_off returns 0 if preset does not exist, otherwise 1)
TSFDEF void tsf_note_off(tsf *f, int preset_index, int key);
TSFDEF int tsf_bank_note_off(tsf *f, int bank, int preset_number, int key);

// Stop playing all notes (end with sustain and release)
TSFDEF void tsf_note_off_all(tsf *f);

// Returns the number of active voices
TSFDEF int tsf_active_voice_count(tsf *f);
TSFDEF const int *tsf_get_active_voice_indices(const tsf *f);
TSFDEF void tsf_get_active_voice_channel_counts(const tsf *f, int *counts,
                                                int count);
TSFDEF void tsf_cleanup_inactive_voices(tsf *f);
TSFDEF void tsf_render_float_indexed(tsf *f, float *buffer, int samples,
                                     int flag_mixing,
                                     const int *voice_indices,
                                     int voice_count);

// Render output samples into a buffer
// You can either render as signed 16-bit values (tsf_render_short) or
// as 32-bit float values (tsf_render_float)
//   buffer: target buffer of size samples * output_channels * sizeof(type)
//   samples: number of samples to render
//   flag_mixing: if 0 clear the buffer first, otherwise mix into existing data
TSFDEF void tsf_render_short(tsf *f, short *buffer, int samples,
                             int flag_mixing CPP_DEFAULT0);
TSFDEF void tsf_render_float(tsf *f, float *buffer, int samples,
                             int flag_mixing CPP_DEFAULT0);

// Higher level channel based functions, set up channel parameters
//   channel: channel number
//   preset_index: preset index >= 0 and < tsf_get_presetcount()
//   preset_number: preset number (alternative to preset_index)
//   flag_mididrums: 0 for normal channels, otherwise apply MIDI drum channel
//   rules bank: instrument bank number (alternative to preset_index) pan:
//   stereo panning value from 0.0 (left) to 1.0 (right) (default 0.5 center)
//   volume: linear volume scale factor (default 1.0 full)
//   pitch_wheel: pitch wheel position 0 to 16383 (default 8192 unpitched)
//   pitch_range: range of the pitch wheel in semitones (default 2.0, total +/-
//   2 semitones) tuning: tuning of all playing voices in semitones (default
//   0.0, standard (A440) tuning) flag_sustain: 0 to end notes that were held
//   sustained and disable holding sustain otherwise enable it
//   (tsf_set_preset_number and set_bank_preset return 0 if preset does not
//   exist, otherwise 1) (tsf_channel_set_... return 0 if a new channel needed
//   allocation and that failed, otherwise 1)
TSFDEF int tsf_channel_set_presetindex(tsf *f, int channel, int preset_index);
TSFDEF int tsf_channel_set_presetnumber(tsf *f, int channel, int preset_number,
                                        int flag_mididrums CPP_DEFAULT0);
TSFDEF int tsf_channel_set_bank(tsf *f, int channel, int bank);
TSFDEF int tsf_channel_set_bank_preset(tsf *f, int channel, int bank,
                                       int preset_number);
TSFDEF int tsf_channel_set_pan(tsf *f, int channel, float pan);
TSFDEF int tsf_channel_set_volume(tsf *f, int channel, float volume);
TSFDEF int tsf_channel_set_pitchwheel(tsf *f, int channel, int pitch_wheel);
TSFDEF int tsf_channel_set_pitchrange(tsf *f, int channel, float pitch_range);
TSFDEF int tsf_channel_set_tuning(tsf *f, int channel, float tuning);
TSFDEF int tsf_channel_set_sustain(tsf *f, int channel, int flag_sustain);

// Start or stop playing notes on a channel (needs channel preset to be set)
//   channel: channel number
//   key: note value between 0 and 127 (60 being middle C)
//   vel: velocity as a float between 0.0 (equal to note off) and 1.0 (full)
//   (tsf_channel_note_on returns 0 on allocation failure of new voice,
//   otherwise 1)
TSFDEF int tsf_channel_note_on(tsf *f, int channel, int key, float vel);
TSFDEF int tsf_channel_note_on_ex(tsf *f, int channel, int key, float gain_vel,
                                  int midi_velocity);
TSFDEF void tsf_channel_note_off(tsf *f, int channel, int key);
TSFDEF void
tsf_channel_note_off_all(tsf *f, int channel); // end with sustain and release
TSFDEF void tsf_channel_sounds_off_all(tsf *f, int channel); // end immediately

// Apply a MIDI control change to the channel (not all controllers are
// supported!)
//    (tsf_channel_midi_control returns 0 on allocation failure of new channel,
//    otherwise 1)
TSFDEF int tsf_channel_midi_control(tsf *f, int channel, int controller,
                                    int control_value);

// Get current values set on the channels
TSFDEF int tsf_channel_get_preset_index(tsf *f, int channel);
TSFDEF int tsf_channel_get_preset_bank(tsf *f, int channel);
TSFDEF int tsf_channel_get_preset_number(tsf *f, int channel);
TSFDEF float tsf_channel_get_pan(tsf *f, int channel);
TSFDEF float tsf_channel_get_volume(tsf *f, int channel);
TSFDEF int tsf_channel_get_pitchwheel(tsf *f, int channel);
TSFDEF float tsf_channel_get_pitchrange(tsf *f, int channel);
TSFDEF float tsf_channel_get_tuning(tsf *f, int channel);

#ifdef __cplusplus
#undef CPP_DEFAULT0
}
#endif

// end header
// ---------------------------------------------------------------------------------------------------------
#endif // TSF_INCLUDE_TSF_INL

#ifdef TSF_IMPLEMENTATION
#undef TSF_IMPLEMENTATION

// SIMD support for voice rendering
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <emmintrin.h> // SSE2
#endif

// The lower this block size is the more accurate the effects are.
// Increasing the value significantly lowers the CPU usage of the voice
// rendering. If LFO affects the low-pass filter it can be hearable even as low
// as 8.
#ifndef TSF_RENDER_EFFECTSAMPLEBLOCK
#define TSF_RENDER_EFFECTSAMPLEBLOCK 64
#endif

// When using tsf_render_short, to do the conversion a buffer of a fixed size is
// allocated on the stack. On low memory platforms this could be made smaller.
// Increasing this above 512 should not have a significant impact on
// performance. The value should be a multiple of TSF_RENDER_EFFECTSAMPLEBLOCK.
#ifndef TSF_RENDER_SHORTBUFFERBLOCK
#define TSF_RENDER_SHORTBUFFERBLOCK 512
#endif

#ifndef TSF_RENDER_MIN_SIMD_SAMPLES
#define TSF_RENDER_MIN_SIMD_SAMPLES 32
#endif

// Grace release time for quick voice off (avoid clicking noise)
#define TSF_FASTRELEASETIME 0.01f

#if !defined(TSF_MALLOC) || !defined(TSF_FREE) || !defined(TSF_REALLOC)
#include <stdlib.h>
#define TSF_MALLOC malloc
#define TSF_FREE free
#define TSF_REALLOC realloc
#endif

#include <stdint.h>

#if !defined(TSF_MEMCPY) || !defined(TSF_MEMSET)
#include <string.h>
#define TSF_MEMCPY memcpy
#define TSF_MEMSET memset
#endif

#if !defined(TSF_POW) || !defined(TSF_POWF) || !defined(TSF_EXPF) ||           \
    !defined(TSF_LOG) || !defined(TSF_TAN) || !defined(TSF_LOG10) ||           \
    !defined(TSF_SQRT)
#include <math.h>
#if !defined(__cplusplus) && !defined(NAN) && !defined(powf) &&                \
    !defined(expf) && !defined(sqrtf)
#define powf (float)pow   // deal with old math.h
#define expf (float)exp   // files that come without
#define sqrtf (float)sqrt // powf, expf and sqrtf
#endif
#define TSF_POW pow
#define TSF_POWF powf
#define TSF_EXPF expf
#define TSF_LOG log
#define TSF_TAN tan
#define TSF_LOG10 log10
#define TSF_SQRTF sqrtf
#endif

#ifndef TSF_NO_STDIO
#include <stdio.h>
#endif

#define TSF_TRUE 1
#define TSF_FALSE 0
#define TSF_BOOL unsigned char
#define TSF_PI 3.14159265358979323846264338327950288
#define TSF_NULL 0
#define TSF_VOICE_STATE_OFF 0
#define TSF_VOICE_STATE_PLAYING 1
#define TSF_VOICE_STATE_SUSTAIN 2
#define TSF_VOICE_STATE_RELEASE 3

#ifdef __cplusplus
extern "C" {
#endif

typedef char tsf_fourcc[4];
typedef signed char tsf_s8;
typedef unsigned char tsf_u8;
typedef unsigned short tsf_u16;
typedef signed short tsf_s16;
typedef unsigned int tsf_u32;
typedef char tsf_char20[20];

#define TSF_FourCCEquals(value1, value2)                                       \
  (value1[0] == value2[0] && value1[1] == value2[1] &&                         \
   value1[2] == value2[2] && value1[3] == value2[3])

enum {
  TSF_STEAL_CLASS_NONE = 0,
  TSF_STEAL_CLASS_ACTIVE_LOUD = 1,
  TSF_STEAL_CLASS_ACTIVE_QUIET = 2,
  TSF_STEAL_CLASS_RELEASE_LOUD = 3,
  TSF_STEAL_CLASS_RELEASE_QUIET = 4
};

#define TSF_STEAL_QUIET_VELOCITY_MAX 60
#define TSF_EXCLUSIVE_GROUP_KEY_EMPTY 0ull
#define TSF_EXCLUSIVE_GROUP_KEY_TOMBSTONE 1ull

struct tsf {
  struct tsf_preset *presets;
  float *fontSamples;
  struct tsf_voice *voices;
  double *voiceSourceSamplePosition;
  double *voiceBasePitchRatio;
  double *voicePitchRatio;
  float *voiceBaseVolumeL;
  float *voiceBaseVolumeR;
  float *voiceVolumeL;
  float *voiceVolumeR;
  float *voiceAmpEnvLevel;
  float *voiceAmpEnvSlope;
  float *voiceModEnvLevel;
  float *voiceModEnvSlope;
  float *voiceModLfoLevel;
  float *voiceModLfoDelta;
  float *voiceVibLfoLevel;
  float *voiceVibLfoDelta;
  float *voiceFilterZ1Left;
  float *voiceFilterZ2Left;
  float *voiceFilterZ1Right;
  float *voiceFilterZ2Right;
  float *voiceFilterA0;
  float *voiceFilterA1;
  float *voiceFilterB1;
  float *voiceFilterB2;
  int *voiceAmpEnvSamplesUntilNextSegment;
  int *voiceModEnvSamplesUntilNextSegment;
  int *voiceModLfoSamplesUntil;
  int *voiceVibLfoSamplesUntil;
  int *voiceFilterActive;
  int *voiceState;
  int *activeVoiceIndices;
  int *freeVoiceIndices;
  struct tsf_channels *channels;
  int channelVoiceHeads[16];
  int channelKeyVoiceHeads[16][128];
  int channelVoiceCounts[16];
  int channelKeyVoiceCounts[16][128];
  unsigned long long *exclusiveGroupKeys;
  int *exclusiveGroupHeads;
  unsigned int exclusiveGroupCapacity;
  unsigned int exclusiveGroupCount;
  unsigned int exclusiveGroupTombstones;
  int stealActiveLoudHead;
  int stealActiveLoudTail;
  int stealActiveQuietHead;
  int stealActiveQuietTail;
  int stealReleaseLoudHead;
  int stealReleaseLoudTail;
  int stealReleaseQuietHead;
  int stealReleaseQuietTail;
  int channelVoiceCacheDirty;

  int presetNum;
  int voiceNum;
  int maxVoiceNum;
  int activeVoiceCount;
  int freeVoiceCount;
  unsigned int voicePlayIndex;

  enum TSFOutputMode outputmode;
  float outSampleRate;
  float globalGainDB;
  int *refCount;
  unsigned int perfHelperContiguousBlocks;
  unsigned int perfHelperGatherBlocks;
  unsigned int perfHelperComplexBlocks;
};

#ifndef TSF_NO_STDIO
static int tsf_stream_stdio_read(FILE *f, void *ptr, unsigned int size) {
  return (int)fread(ptr, 1, size, f);
}
static int tsf_stream_stdio_skip(FILE *f, unsigned int count) {
  return !fseek(f, count, SEEK_CUR);
}
TSFDEF tsf *tsf_load_filename(const char *filename) {
  tsf *res;
  struct tsf_stream stream = {
      TSF_NULL, (int (*)(void *, void *, unsigned int))&tsf_stream_stdio_read,
      (int (*)(void *, unsigned int))&tsf_stream_stdio_skip};
#if __STDC_WANT_SECURE_LIB__
  FILE *f = TSF_NULL;
  fopen_s(&f, filename, "rb");
#else
  FILE *f = fopen(filename, "rb");
#endif
  if (!f) {
    // if (e) *e = TSF_FILENOTFOUND;
    return TSF_NULL;
  }
  stream.data = f;
  res = tsf_load(&stream);
  fclose(f);
  return res;
}
#endif

struct tsf_stream_memory {
  const char *buffer;
  unsigned int total, pos;
};
static int tsf_stream_memory_read(struct tsf_stream_memory *m, void *ptr,
                                  unsigned int size) {
  if (size > m->total - m->pos)
    size = m->total - m->pos;
  TSF_MEMCPY(ptr, m->buffer + m->pos, size);
  m->pos += size;
  return size;
}
static int tsf_stream_memory_skip(struct tsf_stream_memory *m,
                                  unsigned int count) {
  if (m->pos + count > m->total)
    return 0;
  m->pos += count;
  return 1;
}
TSFDEF tsf *tsf_load_memory(const void *buffer, int size) {
  struct tsf_stream stream = {
      TSF_NULL, (int (*)(void *, void *, unsigned int))&tsf_stream_memory_read,
      (int (*)(void *, unsigned int))&tsf_stream_memory_skip};
  struct tsf_stream_memory f = {0, 0, 0};
  f.buffer = (const char *)buffer;
  f.total = size;
  stream.data = &f;
  return tsf_load(&stream);
}

enum { TSF_LOOPMODE_NONE, TSF_LOOPMODE_CONTINUOUS, TSF_LOOPMODE_SUSTAIN };

enum {
  TSF_SEGMENT_NONE,
  TSF_SEGMENT_DELAY,
  TSF_SEGMENT_ATTACK,
  TSF_SEGMENT_HOLD,
  TSF_SEGMENT_DECAY,
  TSF_SEGMENT_SUSTAIN,
  TSF_SEGMENT_RELEASE,
  TSF_SEGMENT_DONE
};

struct tsf_hydra {
  struct tsf_hydra_phdr *phdrs;
  struct tsf_hydra_pbag *pbags;
  struct tsf_hydra_pmod *pmods;
  struct tsf_hydra_pgen *pgens;
  struct tsf_hydra_inst *insts;
  struct tsf_hydra_ibag *ibags;
  struct tsf_hydra_imod *imods;
  struct tsf_hydra_igen *igens;
  struct tsf_hydra_shdr *shdrs;
  int phdrNum, pbagNum, pmodNum, pgenNum, instNum, ibagNum, imodNum, igenNum,
      shdrNum;
};

union tsf_hydra_genamount {
  struct {
    tsf_u8 lo, hi;
  } range;
  tsf_s16 shortAmount;
  tsf_u16 wordAmount;
};
struct tsf_hydra_phdr {
  tsf_char20 presetName;
  tsf_u16 preset, bank, presetBagNdx;
  tsf_u32 library, genre, morphology;
};
struct tsf_hydra_pbag {
  tsf_u16 genNdx, modNdx;
};
struct tsf_hydra_pmod {
  tsf_u16 modSrcOper, modDestOper;
  tsf_s16 modAmount;
  tsf_u16 modAmtSrcOper, modTransOper;
};
struct tsf_hydra_pgen {
  tsf_u16 genOper;
  union tsf_hydra_genamount genAmount;
};
struct tsf_hydra_inst {
  tsf_char20 instName;
  tsf_u16 instBagNdx;
};
struct tsf_hydra_ibag {
  tsf_u16 instGenNdx, instModNdx;
};
struct tsf_hydra_imod {
  tsf_u16 modSrcOper, modDestOper;
  tsf_s16 modAmount;
  tsf_u16 modAmtSrcOper, modTransOper;
};
struct tsf_hydra_igen {
  tsf_u16 genOper;
  union tsf_hydra_genamount genAmount;
};
struct tsf_hydra_shdr {
  tsf_char20 sampleName;
  tsf_u32 start, end, startLoop, endLoop, sampleRate;
  tsf_u8 originalPitch;
  tsf_s8 pitchCorrection;
  tsf_u16 sampleLink, sampleType;
};

#define TSFR(FIELD) stream->read(stream->data, &i->FIELD, sizeof(i->FIELD));
static void tsf_hydra_read_phdr(struct tsf_hydra_phdr *i,
                                struct tsf_stream *stream) {
  TSFR(presetName)
  TSFR(preset) TSFR(bank) TSFR(presetBagNdx) TSFR(library) TSFR(genre)
      TSFR(morphology)
}
static void tsf_hydra_read_pbag(struct tsf_hydra_pbag *i,
                                struct tsf_stream *stream) {
  TSFR(genNdx) TSFR(modNdx)
}
static void tsf_hydra_read_pmod(struct tsf_hydra_pmod *i,
                                struct tsf_stream *stream) {
  TSFR(modSrcOper)
  TSFR(modDestOper) TSFR(modAmount) TSFR(modAmtSrcOper) TSFR(modTransOper)
}
static void tsf_hydra_read_pgen(struct tsf_hydra_pgen *i,
                                struct tsf_stream *stream) {
  TSFR(genOper) TSFR(genAmount)
}
static void tsf_hydra_read_inst(struct tsf_hydra_inst *i,
                                struct tsf_stream *stream) {
  TSFR(instName) TSFR(instBagNdx)
}
static void tsf_hydra_read_ibag(struct tsf_hydra_ibag *i,
                                struct tsf_stream *stream) {
  TSFR(instGenNdx) TSFR(instModNdx)
}
static void tsf_hydra_read_imod(struct tsf_hydra_imod *i,
                                struct tsf_stream *stream) {
  TSFR(modSrcOper)
  TSFR(modDestOper) TSFR(modAmount) TSFR(modAmtSrcOper) TSFR(modTransOper)
}
static void tsf_hydra_read_igen(struct tsf_hydra_igen *i,
                                struct tsf_stream *stream) {
  TSFR(genOper) TSFR(genAmount)
}
static void tsf_hydra_read_shdr(struct tsf_hydra_shdr *i,
                                struct tsf_stream *stream) {
  TSFR(sampleName)
  TSFR(start) TSFR(end) TSFR(startLoop) TSFR(endLoop) TSFR(sampleRate)
      TSFR(originalPitch) TSFR(pitchCorrection) TSFR(sampleLink)
          TSFR(sampleType)
}
#undef TSFR

struct tsf_riffchunk {
  tsf_fourcc id;
  tsf_u32 size;
};
struct tsf_envelope {
  float delay, attack, hold, decay, sustain, release, keynumToHold,
      keynumToDecay;
};
struct tsf_voice_envelope {
  unsigned char segment, segmentIsExponential : 1, isAmpEnv : 1;
  short midiVelocity;
  float level, slope;
  int samplesUntilNextSegment;
  struct tsf_envelope parameters;
};
struct tsf_voice_lowpass {
  double QInv, a0, a1, b1, b2, z1, z2;
  TSF_BOOL active;
};
struct tsf_voice_lfo {
  int samplesUntil;
  float level, delta;
};

struct tsf_region {
  int loop_mode;
  unsigned int sample_rate;
  unsigned char lokey, hikey, lovel, hivel;
  unsigned int group, offset, end, loop_start, loop_end;
  int transpose, tune, pitch_keycenter, pitch_keytrack;
  float attenuation, pan;
  struct tsf_envelope ampenv, modenv;
  int initialFilterQ, initialFilterFc;
  int modEnvToPitch, modEnvToFilterFc, modLfoToFilterFc, modLfoToVolume;
  float delayModLFO;
  int freqModLFO, modLfoToPitch;
  float delayVibLFO;
  int freqVibLFO, vibLfoToPitch;
};

struct tsf_preset {
  tsf_char20 presetName;
  tsf_u16 preset, bank;
  struct tsf_region *regions;
  int *keyRegionIndices;
  int keyRegionOffsets[128];
  int keyRegionCounts[128];
  int regionNum;
};

struct tsf_voice {
  int playingPreset, playingKey, playingChannel, heldSustain;
  int pendingFree;
  int nextChannelVoice;
  int nextChannelKeyVoice;
  int nextExclusiveGroupVoice;
  int prevExclusiveGroupVoice;
  int exclusiveGroupSlot;
  int nextStealVoice;
  int prevStealVoice;
  int stealClass;
  struct tsf_region *region;
  double pitchInputTimecents, pitchOutputFactor;
  double sourceSamplePosition;
  float noteGainDB, panFactorLeft, panFactorRight;
  short midiVelocity;
  unsigned int playIndex, loopStart, loopEnd;
  struct tsf_voice_envelope ampenv, modenv;
  struct tsf_voice_lowpass lowpass;
  struct tsf_voice_lfo modlfo, viblfo;
};

struct tsf_channel {
  unsigned short presetIndex, bank, pitchWheel, midiPan, midiVolume,
      midiExpression, midiRPN, midiData : 14, sustain : 1;
  float panOffset, gainDB, pitchRange, tuning;
};

struct tsf_channels {
  void (*setupVoice)(tsf *f, struct tsf_voice *voice);
  int channelNum, activeChannel;
  struct tsf_channel channels[1];
};

static double tsf_timecents2Secsd(double timecents) {
  return TSF_POW(2.0, timecents / 1200.0);
}
static float tsf_timecents2Secsf(float timecents) {
  return TSF_POWF(2.0f, timecents / 1200.0f);
}
static float tsf_cents2Hertz(float cents) {
  return 8.176f * TSF_POWF(2.0f, cents / 1200.0f);
}
static float tsf_decibelsToGain(float db) {
  return (db > -100.f ? TSF_POWF(10.0f, db * 0.05f) : 0);
}
static float tsf_gainToDecibels(float gain) {
  return (gain <= .00001f ? -100.f : (float)(20.0 * TSF_LOG10(gain)));
}

static TSF_BOOL tsf_riffchunk_read(struct tsf_riffchunk *parent,
                                   struct tsf_riffchunk *chunk,
                                   struct tsf_stream *stream) {
  TSF_BOOL IsRiff, IsList;
  if (parent && sizeof(tsf_fourcc) + sizeof(tsf_u32) > parent->size)
    return TSF_FALSE;
  if (!stream->read(stream->data, &chunk->id, sizeof(tsf_fourcc)) ||
      *chunk->id <= ' ' || *chunk->id >= 'z')
    return TSF_FALSE;
  if (!stream->read(stream->data, &chunk->size, sizeof(tsf_u32)))
    return TSF_FALSE;
  if (parent &&
      sizeof(tsf_fourcc) + sizeof(tsf_u32) + chunk->size > parent->size)
    return TSF_FALSE;
  if (parent)
    parent->size -= sizeof(tsf_fourcc) + sizeof(tsf_u32) + chunk->size;
  IsRiff = TSF_FourCCEquals(chunk->id, "RIFF"),
  IsList = TSF_FourCCEquals(chunk->id, "LIST");
  if (IsRiff && parent)
    return TSF_FALSE; // not allowed
  if (!IsRiff && !IsList)
    return TSF_TRUE; // custom type without sub type
  if (!stream->read(stream->data, &chunk->id, sizeof(tsf_fourcc)) ||
      *chunk->id <= ' ' || *chunk->id >= 'z')
    return TSF_FALSE;
  chunk->size -= sizeof(tsf_fourcc);
  return TSF_TRUE;
}

static void tsf_region_clear(struct tsf_region *i, TSF_BOOL for_relative) {
  TSF_MEMSET(i, 0, sizeof(struct tsf_region));
  i->hikey = i->hivel = 127;
  i->pitch_keycenter = 60; // C4
  if (for_relative)
    return;

  i->pitch_keytrack = 100;

  i->pitch_keycenter = -1;

  // SF2 defaults in timecents.
  i->ampenv.delay = i->ampenv.attack = i->ampenv.hold = i->ampenv.decay =
      i->ampenv.release = -12000.0f;
  i->modenv.delay = i->modenv.attack = i->modenv.hold = i->modenv.decay =
      i->modenv.release = -12000.0f;

  i->initialFilterFc = 13500;

  i->delayModLFO = -12000.0f;
  i->delayVibLFO = -12000.0f;
}

static void tsf_region_operator(struct tsf_region *region, tsf_u16 genOper,
                                union tsf_hydra_genamount *amount,
                                struct tsf_region *merge_region) {
  enum {
    _GEN_TYPE_MASK = 0x0F,
    GEN_FLOAT = 0x01,
    GEN_INT = 0x02,
    GEN_UINT_ADD = 0x03,
    GEN_UINT_ADD15 = 0x04,
    GEN_KEYRANGE = 0x05,
    GEN_VELRANGE = 0x06,
    GEN_LOOPMODE = 0x07,
    GEN_GROUP = 0x08,
    GEN_KEYCENTER = 0x09,

    _GEN_LIMIT_MASK = 0xF0,
    GEN_INT_LIMIT12K = 0x10,     // min -12000, max 12000
    GEN_INT_LIMITFC = 0x20,      // min 1500, max 13500
    GEN_INT_LIMITQ = 0x30,       // min 0, max 960
    GEN_INT_LIMIT960 = 0x40,     // min -960, max 960
    GEN_INT_LIMIT16K4500 = 0x50, // min -16000, max 4500
    GEN_FLOAT_LIMIT12K5K = 0x60, // min -12000, max 5000
    GEN_FLOAT_LIMIT12K8K = 0x70, // min -12000, max 8000
    GEN_FLOAT_LIMIT1200 = 0x80,  // min -1200, max 1200
    GEN_FLOAT_LIMITPAN = 0x90,   //* .001f, min -.5f, max .5f,
    GEN_FLOAT_LIMITATTN = 0xA0,  //* .1f, min 0, max 144.0
    GEN_FLOAT_MAX1000 = 0xB0,    // min 0, max 1000
    GEN_FLOAT_MAX1440 = 0xC0,    // min 0, max 1440

    _GEN_MAX = 59
  };
#define _TSFREGIONOFFSET(TYPE, FIELD)                                          \
  (unsigned char)(((TYPE *)&((struct tsf_region *)0)->FIELD) - (TYPE *)0)
#define _TSFREGIONENVOFFSET(TYPE, ENV, FIELD)                                  \
  (unsigned char)(((TYPE *)&((&(((struct tsf_region *)0)->ENV))->FIELD)) -     \
                  (TYPE *)0)
  static const struct {
    unsigned char mode, offset;
  } genMetas[_GEN_MAX] = {
      {GEN_UINT_ADD,
       _TSFREGIONOFFSET(unsigned int, offset)},            // 0 StartAddrsOffset
      {GEN_UINT_ADD, _TSFREGIONOFFSET(unsigned int, end)}, // 1 EndAddrsOffset
      {GEN_UINT_ADD,
       _TSFREGIONOFFSET(unsigned int, loop_start)}, // 2 StartloopAddrsOffset
      {GEN_UINT_ADD,
       _TSFREGIONOFFSET(unsigned int, loop_end)}, // 3 EndloopAddrsOffset
      {GEN_UINT_ADD15,
       _TSFREGIONOFFSET(unsigned int, offset)}, // 4 StartAddrsCoarseOffset
      {GEN_INT | GEN_INT_LIMIT12K,
       _TSFREGIONOFFSET(int, modLfoToPitch)}, // 5 ModLfoToPitch
      {GEN_INT | GEN_INT_LIMIT12K,
       _TSFREGIONOFFSET(int, vibLfoToPitch)}, // 6 VibLfoToPitch
      {GEN_INT | GEN_INT_LIMIT12K,
       _TSFREGIONOFFSET(int, modEnvToPitch)}, // 7 ModEnvToPitch
      {GEN_INT | GEN_INT_LIMITFC,
       _TSFREGIONOFFSET(int, initialFilterFc)}, // 8 InitialFilterFc
      {GEN_INT | GEN_INT_LIMITQ,
       _TSFREGIONOFFSET(int, initialFilterQ)}, // 9 InitialFilterQ
      {GEN_INT | GEN_INT_LIMIT12K,
       _TSFREGIONOFFSET(int, modLfoToFilterFc)}, // 10 ModLfoToFilterFc
      {GEN_INT | GEN_INT_LIMIT12K,
       _TSFREGIONOFFSET(int, modEnvToFilterFc)}, // 11 ModEnvToFilterFc
      {GEN_UINT_ADD15,
       _TSFREGIONOFFSET(unsigned int, end)}, // 12 EndAddrsCoarseOffset
      {GEN_INT | GEN_INT_LIMIT960,
       _TSFREGIONOFFSET(int, modLfoToVolume)}, // 13 ModLfoToVolume
      {0, (0)},                                //   Unused
      {0, (0)}, // 15 ChorusEffectsSend (unsupported)
      {0, (0)}, // 16 ReverbEffectsSend (unsupported)
      {GEN_FLOAT | GEN_FLOAT_LIMITPAN, _TSFREGIONOFFSET(float, pan)}, // 17 Pan
      {0, (0)}, //   Unused
      {0, (0)}, //   Unused
      {0, (0)}, //   Unused
      {GEN_FLOAT | GEN_FLOAT_LIMIT12K5K,
       _TSFREGIONOFFSET(float, delayModLFO)}, // 21 DelayModLFO
      {GEN_INT | GEN_INT_LIMIT16K4500,
       _TSFREGIONOFFSET(int, freqModLFO)}, // 22 FreqModLFO
      {GEN_FLOAT | GEN_FLOAT_LIMIT12K5K,
       _TSFREGIONOFFSET(float, delayVibLFO)}, // 23 DelayVibLFO
      {GEN_INT | GEN_INT_LIMIT16K4500,
       _TSFREGIONOFFSET(int, freqVibLFO)}, // 24 FreqVibLFO
      {GEN_FLOAT | GEN_FLOAT_LIMIT12K5K,
       _TSFREGIONENVOFFSET(float, modenv, delay)}, // 25 DelayModEnv
      {GEN_FLOAT | GEN_FLOAT_LIMIT12K8K,
       _TSFREGIONENVOFFSET(float, modenv, attack)}, // 26 AttackModEnv
      {GEN_FLOAT | GEN_FLOAT_LIMIT12K5K,
       _TSFREGIONENVOFFSET(float, modenv, hold)}, // 27 HoldModEnv
      {GEN_FLOAT | GEN_FLOAT_LIMIT12K8K,
       _TSFREGIONENVOFFSET(float, modenv, decay)}, // 28 DecayModEnv
      {GEN_FLOAT | GEN_FLOAT_MAX1000,
       _TSFREGIONENVOFFSET(float, modenv, sustain)}, // 29 SustainModEnv
      {GEN_FLOAT | GEN_FLOAT_LIMIT12K8K,
       _TSFREGIONENVOFFSET(float, modenv, release)}, // 30 ReleaseModEnv
      {GEN_FLOAT | GEN_FLOAT_LIMIT1200,
       _TSFREGIONENVOFFSET(float, modenv,
                           keynumToHold)}, // 31 KeynumToModEnvHold
      {GEN_FLOAT | GEN_FLOAT_LIMIT1200,
       _TSFREGIONENVOFFSET(float, modenv,
                           keynumToDecay)}, // 32 KeynumToModEnvDecay
      {GEN_FLOAT | GEN_FLOAT_LIMIT12K5K,
       _TSFREGIONENVOFFSET(float, ampenv, delay)}, // 33 DelayVolEnv
      {GEN_FLOAT | GEN_FLOAT_LIMIT12K8K,
       _TSFREGIONENVOFFSET(float, ampenv, attack)}, // 34 AttackVolEnv
      {GEN_FLOAT | GEN_FLOAT_LIMIT12K5K,
       _TSFREGIONENVOFFSET(float, ampenv, hold)}, // 35 HoldVolEnv
      {GEN_FLOAT | GEN_FLOAT_LIMIT12K8K,
       _TSFREGIONENVOFFSET(float, ampenv, decay)}, // 36 DecayVolEnv
      {GEN_FLOAT | GEN_FLOAT_MAX1440,
       _TSFREGIONENVOFFSET(float, ampenv, sustain)}, // 37 SustainVolEnv
      {GEN_FLOAT | GEN_FLOAT_LIMIT12K8K,
       _TSFREGIONENVOFFSET(float, ampenv, release)}, // 38 ReleaseVolEnv
      {GEN_FLOAT | GEN_FLOAT_LIMIT1200,
       _TSFREGIONENVOFFSET(float, ampenv,
                           keynumToHold)}, // 39 KeynumToVolEnvHold
      {GEN_FLOAT | GEN_FLOAT_LIMIT1200,
       _TSFREGIONENVOFFSET(float, ampenv,
                           keynumToDecay)}, // 40 KeynumToVolEnvDecay
      {0, (0)},                             //   Instrument (special)
      {0, (0)},                             //   Reserved
      {GEN_KEYRANGE, (0)},                  // 43 KeyRange
      {GEN_VELRANGE, (0)},                  // 44 VelRange
      {GEN_UINT_ADD15,
       _TSFREGIONOFFSET(unsigned int,
                        loop_start)}, // 45 StartloopAddrsCoarseOffset
      {0, (0)},                       // 46 Keynum (special)
      {0, (0)},                       // 47 Velocity (special)
      {GEN_FLOAT | GEN_FLOAT_LIMITATTN,
       _TSFREGIONOFFSET(float, attenuation)}, // 48 InitialAttenuation
      {0, (0)},                               //   Reserved
      {GEN_UINT_ADD15,
       _TSFREGIONOFFSET(unsigned int, loop_end)}, // 50 EndloopAddrsCoarseOffset
      {GEN_INT, _TSFREGIONOFFSET(int, transpose)},      // 51 CoarseTune
      {GEN_INT, _TSFREGIONOFFSET(int, tune)},           // 52 FineTune
      {0, (0)},                                         //   SampleID (special)
      {GEN_LOOPMODE, _TSFREGIONOFFSET(int, loop_mode)}, // 54 SampleModes
      {0, (0)},                                         //   Reserved
      {GEN_INT, _TSFREGIONOFFSET(int, pitch_keytrack)}, // 56 ScaleTuning
      {GEN_GROUP, _TSFREGIONOFFSET(unsigned int, group)}, // 57 ExclusiveClass
      {GEN_KEYCENTER,
       _TSFREGIONOFFSET(int, pitch_keycenter)}, // 58 OverridingRootKey
  };
#undef _TSFREGIONOFFSET
#undef _TSFREGIONENVOFFSET
  if (amount) {
    int offset;
    if (genOper >= _GEN_MAX)
      return;
    offset = genMetas[genOper].offset;
    switch (genMetas[genOper].mode & _GEN_TYPE_MASK) {
    case GEN_FLOAT:
      ((float *)region)[offset] = amount->shortAmount;
      return;
    case GEN_INT:
      ((int *)region)[offset] = amount->shortAmount;
      return;
    case GEN_UINT_ADD:
      ((unsigned int *)region)[offset] += amount->shortAmount;
      return;
    case GEN_UINT_ADD15:
      ((unsigned int *)region)[offset] += amount->shortAmount << 15;
      return;
    case GEN_KEYRANGE:
      region->lokey = amount->range.lo;
      region->hikey = amount->range.hi;
      return;
    case GEN_VELRANGE:
      region->lovel = amount->range.lo;
      region->hivel = amount->range.hi;
      return;
    case GEN_LOOPMODE:
      region->loop_mode =
          ((amount->wordAmount & 3) == 3
               ? TSF_LOOPMODE_SUSTAIN
               : ((amount->wordAmount & 3) == 1 ? TSF_LOOPMODE_CONTINUOUS
                                                : TSF_LOOPMODE_NONE));
      return;
    case GEN_GROUP:
      region->group = amount->wordAmount;
      return;
    case GEN_KEYCENTER:
      region->pitch_keycenter = amount->shortAmount;
      return;
    }
  } else // merge regions and clamp values
  {
    for (genOper = 0; genOper != _GEN_MAX; genOper++) {
      int offset = genMetas[genOper].offset;
      switch (genMetas[genOper].mode & _GEN_TYPE_MASK) {
      case GEN_FLOAT: {
        float *val = &((float *)region)[offset], vfactor, vmin, vmax;
        *val += ((float *)merge_region)[offset];
        switch (genMetas[genOper].mode & _GEN_LIMIT_MASK) {
        case GEN_FLOAT_LIMIT12K5K:
          vfactor = 1.0f;
          vmin = -12000.0f;
          vmax = 5000.0f;
          break;
        case GEN_FLOAT_LIMIT12K8K:
          vfactor = 1.0f;
          vmin = -12000.0f;
          vmax = 8000.0f;
          break;
        case GEN_FLOAT_LIMIT1200:
          vfactor = 1.0f;
          vmin = -1200.0f;
          vmax = 1200.0f;
          break;
        case GEN_FLOAT_LIMITPAN:
          vfactor = 0.001f;
          vmin = -0.5f;
          vmax = 0.5f;
          break;
        case GEN_FLOAT_LIMITATTN:
          vfactor = 0.01f;
          vmin = 0.0f;
          vmax = 14.4f;
          break;
        case GEN_FLOAT_MAX1000:
          vfactor = 1.0f;
          vmin = 0.0f;
          vmax = 1000.0f;
          break;
        case GEN_FLOAT_MAX1440:
          vfactor = 1.0f;
          vmin = 0.0f;
          vmax = 1440.0f;
          break;
        default:
          continue;
        }
        *val *= vfactor;
        if (*val < vmin)
          *val = vmin;
        else if (*val > vmax)
          *val = vmax;
        continue;
      }
      case GEN_INT: {
        int *val = &((int *)region)[offset], vmin, vmax;
        *val += ((int *)merge_region)[offset];
        switch (genMetas[genOper].mode & _GEN_LIMIT_MASK) {
        case GEN_INT_LIMIT12K:
          vmin = -12000;
          vmax = 12000;
          break;
        case GEN_INT_LIMITFC:
          vmin = 1500;
          vmax = 13500;
          break;
        case GEN_INT_LIMITQ:
          vmin = 0;
          vmax = 960;
          break;
        case GEN_INT_LIMIT960:
          vmin = -960;
          vmax = 960;
          break;
        case GEN_INT_LIMIT16K4500:
          vmin = -16000;
          vmax = 4500;
          break;
        default:
          continue;
        }
        if (*val < vmin)
          *val = vmin;
        else if (*val > vmax)
          *val = vmax;
        continue;
      }
      case GEN_UINT_ADD: {
        ((unsigned int *)region)[offset] +=
            ((unsigned int *)merge_region)[offset];
        continue;
      }
      }
    }
  }
}

static void tsf_region_envtosecs(struct tsf_envelope *p,
                                 TSF_BOOL sustainIsGain) {
  // EG times need to be converted from timecents to seconds.
  // Pin very short EG segments.  Timecents don't get to zero, and our EG is
  // happier with zero values.
  p->delay = (p->delay < -11950.0f ? 0.0f : tsf_timecents2Secsf(p->delay));
  p->attack = (p->attack < -11950.0f ? 0.0f : tsf_timecents2Secsf(p->attack));
  p->release =
      (p->release < -11950.0f ? 0.0f : tsf_timecents2Secsf(p->release));

  // If we have dynamic hold or decay times depending on key number we need
  // to keep the values in timecents so we can calculate it during startNote
  if (!p->keynumToHold)
    p->hold = (p->hold < -11950.0f ? 0.0f : tsf_timecents2Secsf(p->hold));
  if (!p->keynumToDecay)
    p->decay = (p->decay < -11950.0f ? 0.0f : tsf_timecents2Secsf(p->decay));

  if (p->sustain < 0.0f)
    p->sustain = 0.0f;
  else if (sustainIsGain)
    p->sustain = tsf_decibelsToGain(-p->sustain / 10.0f);
  else
    p->sustain = 1.0f - (p->sustain / 1000.0f);
}

static void tsf_preset_init_key_dispatch(struct tsf_preset *preset) {
  int key;
  preset->keyRegionIndices = TSF_NULL;
  for (key = 0; key < 128; ++key) {
    preset->keyRegionOffsets[key] = 0;
    preset->keyRegionCounts[key] = 0;
  }
}

static void tsf_preset_free_key_dispatch(struct tsf_preset *preset) {
  if (preset->keyRegionIndices) {
    TSF_FREE(preset->keyRegionIndices);
    preset->keyRegionIndices = TSF_NULL;
  }
}

static int tsf_preset_build_key_dispatch(struct tsf_preset *preset) {
  int totalEntries = 0;
  int key, regionIndex;
  int *writeOffsets;

  if (!preset || !preset->regions || preset->regionNum <= 0)
    return 1;

  for (regionIndex = 0; regionIndex < preset->regionNum; ++regionIndex) {
    struct tsf_region *region = &preset->regions[regionIndex];
    int lowKey = region->lokey;
    int highKey = region->hikey;
    if (lowKey < 0)
      lowKey = 0;
    if (highKey > 127)
      highKey = 127;
    if (highKey < lowKey)
      continue;
    for (key = lowKey; key <= highKey; ++key) {
      preset->keyRegionCounts[key]++;
      totalEntries++;
    }
  }

  if (totalEntries <= 0)
    return 1;

  preset->keyRegionIndices = (int *)TSF_MALLOC(totalEntries * sizeof(int));
  if (!preset->keyRegionIndices)
    return 0;

  writeOffsets = (int *)TSF_MALLOC(128 * sizeof(int));
  if (!writeOffsets) {
    tsf_preset_free_key_dispatch(preset);
    return 0;
  }

  totalEntries = 0;
  for (key = 0; key < 128; ++key) {
    preset->keyRegionOffsets[key] = totalEntries;
    writeOffsets[key] = totalEntries;
    totalEntries += preset->keyRegionCounts[key];
  }

  for (regionIndex = 0; regionIndex < preset->regionNum; ++regionIndex) {
    struct tsf_region *region = &preset->regions[regionIndex];
    int lowKey = region->lokey;
    int highKey = region->hikey;
    if (lowKey < 0)
      lowKey = 0;
    if (highKey > 127)
      highKey = 127;
    if (highKey < lowKey)
      continue;
    for (key = lowKey; key <= highKey; ++key)
      preset->keyRegionIndices[writeOffsets[key]++] = regionIndex;
  }

  TSF_FREE(writeOffsets);
  return 1;
}

static int tsf_load_presets(tsf *res, struct tsf_hydra *hydra,
                            unsigned int fontSampleCount) {
  enum {
    GenInstrument = 41,
    GenKeyRange = 43,
    GenVelRange = 44,
    GenSampleID = 53
  };
  // Read each preset.
  struct tsf_hydra_phdr *pphdr, *pphdrMax;
  res->presetNum = hydra->phdrNum - 1;
  res->presets = (struct tsf_preset *)TSF_MALLOC(res->presetNum *
                                                 sizeof(struct tsf_preset));
  if (!res->presets)
    return 0;
  else {
    int i;
    for (i = 0; i != res->presetNum; i++) {
      res->presets[i].regions = TSF_NULL;
      tsf_preset_init_key_dispatch(&res->presets[i]);
    }
  }
  for (pphdr = hydra->phdrs, pphdrMax = pphdr + hydra->phdrNum - 1;
       pphdr != pphdrMax; pphdr++) {
    int sortedIndex = 0, region_index = 0;
    struct tsf_hydra_phdr *otherphdr;
    struct tsf_preset *preset;
    struct tsf_hydra_pbag *ppbag, *ppbagEnd;
    struct tsf_region globalRegion;
    for (otherphdr = hydra->phdrs; otherphdr != pphdrMax; otherphdr++) {
      if (otherphdr == pphdr || otherphdr->bank > pphdr->bank)
        continue;
      else if (otherphdr->bank < pphdr->bank)
        sortedIndex++;
      else if (otherphdr->preset > pphdr->preset)
        continue;
      else if (otherphdr->preset < pphdr->preset)
        sortedIndex++;
      else if (otherphdr < pphdr)
        sortedIndex++;
    }

    preset = &res->presets[sortedIndex];
    TSF_MEMCPY(preset->presetName, pphdr->presetName,
               sizeof(preset->presetName));
    preset->presetName[sizeof(preset->presetName) - 1] =
        '\0'; // should be zero terminated in source file but make sure
    preset->bank = pphdr->bank;
    preset->preset = pphdr->preset;
    preset->regionNum = 0;

    // count regions covered by this preset
    for (ppbag = hydra->pbags + pphdr->presetBagNdx,
        ppbagEnd = hydra->pbags + pphdr[1].presetBagNdx;
         ppbag != ppbagEnd; ppbag++) {
      unsigned char plokey = 0, phikey = 127, plovel = 0, phivel = 127;
      struct tsf_hydra_pgen *ppgen, *ppgenEnd;
      struct tsf_hydra_inst *pinst;
      struct tsf_hydra_ibag *pibag, *pibagEnd;
      struct tsf_hydra_igen *pigen, *pigenEnd;
      for (ppgen = hydra->pgens + ppbag->genNdx,
          ppgenEnd = hydra->pgens + ppbag[1].genNdx;
           ppgen != ppgenEnd; ppgen++) {
        if (ppgen->genOper == GenKeyRange) {
          plokey = ppgen->genAmount.range.lo;
          phikey = ppgen->genAmount.range.hi;
          continue;
        }
        if (ppgen->genOper == GenVelRange) {
          plovel = ppgen->genAmount.range.lo;
          phivel = ppgen->genAmount.range.hi;
          continue;
        }
        if (ppgen->genOper != GenInstrument)
          continue;
        if (ppgen->genAmount.wordAmount >= hydra->instNum)
          continue;
        pinst = hydra->insts + ppgen->genAmount.wordAmount;
        for (pibag = hydra->ibags + pinst->instBagNdx,
            pibagEnd = hydra->ibags + pinst[1].instBagNdx;
             pibag != pibagEnd; pibag++) {
          unsigned char ilokey = 0, ihikey = 127, ilovel = 0, ihivel = 127;
          for (pigen = hydra->igens + pibag->instGenNdx,
              pigenEnd = hydra->igens + pibag[1].instGenNdx;
               pigen != pigenEnd; pigen++) {
            if (pigen->genOper == GenKeyRange) {
              ilokey = pigen->genAmount.range.lo;
              ihikey = pigen->genAmount.range.hi;
              continue;
            }
            if (pigen->genOper == GenVelRange) {
              ilovel = pigen->genAmount.range.lo;
              ihivel = pigen->genAmount.range.hi;
              continue;
            }
            if (pigen->genOper == GenSampleID && ihikey >= plokey &&
                ilokey <= phikey && ihivel >= plovel && ilovel <= phivel)
              preset->regionNum++;
          }
        }
      }
    }

    preset->regions = (struct tsf_region *)TSF_MALLOC(
        preset->regionNum * sizeof(struct tsf_region));
    if (!preset->regions) {
      int i;
      for (i = 0; i != res->presetNum; i++) {
        TSF_FREE(res->presets[i].regions);
        tsf_preset_free_key_dispatch(&res->presets[i]);
      }
      TSF_FREE(res->presets);
      return 0;
    }
    tsf_region_clear(&globalRegion, TSF_TRUE);

    // Zones.
    for (ppbag = hydra->pbags + pphdr->presetBagNdx,
        ppbagEnd = hydra->pbags + pphdr[1].presetBagNdx;
         ppbag != ppbagEnd; ppbag++) {
      struct tsf_hydra_pgen *ppgen, *ppgenEnd;
      struct tsf_hydra_inst *pinst;
      struct tsf_hydra_ibag *pibag, *pibagEnd;
      struct tsf_hydra_igen *pigen, *pigenEnd;
      struct tsf_region presetRegion = globalRegion;
      int hadGenInstrument = 0;

      // Generators.
      for (ppgen = hydra->pgens + ppbag->genNdx,
          ppgenEnd = hydra->pgens + ppbag[1].genNdx;
           ppgen != ppgenEnd; ppgen++) {
        // Instrument.
        if (ppgen->genOper == GenInstrument) {
          struct tsf_region instRegion;
          tsf_u16 whichInst = ppgen->genAmount.wordAmount;
          if (whichInst >= hydra->instNum)
            continue;

          tsf_region_clear(&instRegion, TSF_FALSE);
          pinst = &hydra->insts[whichInst];
          for (pibag = hydra->ibags + pinst->instBagNdx,
              pibagEnd = hydra->ibags + pinst[1].instBagNdx;
               pibag != pibagEnd; pibag++) {
            // Generators.
            struct tsf_region zoneRegion = instRegion;
            int hadSampleID = 0;
            for (pigen = hydra->igens + pibag->instGenNdx,
                pigenEnd = hydra->igens + pibag[1].instGenNdx;
                 pigen != pigenEnd; pigen++) {
              if (pigen->genOper == GenSampleID) {
                struct tsf_hydra_shdr *pshdr;

                // preset region key and vel ranges are a filter for the zone
                // regions
                if (zoneRegion.hikey < presetRegion.lokey ||
                    zoneRegion.lokey > presetRegion.hikey)
                  continue;
                if (zoneRegion.hivel < presetRegion.lovel ||
                    zoneRegion.lovel > presetRegion.hivel)
                  continue;
                if (presetRegion.lokey > zoneRegion.lokey)
                  zoneRegion.lokey = presetRegion.lokey;
                if (presetRegion.hikey < zoneRegion.hikey)
                  zoneRegion.hikey = presetRegion.hikey;
                if (presetRegion.lovel > zoneRegion.lovel)
                  zoneRegion.lovel = presetRegion.lovel;
                if (presetRegion.hivel < zoneRegion.hivel)
                  zoneRegion.hivel = presetRegion.hivel;

                // sum regions
                tsf_region_operator(&zoneRegion, 0, TSF_NULL, &presetRegion);

                // EG times need to be converted from timecents to seconds.
                tsf_region_envtosecs(&zoneRegion.ampenv, TSF_TRUE);
                tsf_region_envtosecs(&zoneRegion.modenv, TSF_FALSE);

                // LFO times need to be converted from timecents to seconds.
                zoneRegion.delayModLFO =
                    (zoneRegion.delayModLFO < -11950.0f
                         ? 0.0f
                         : tsf_timecents2Secsf(zoneRegion.delayModLFO));
                zoneRegion.delayVibLFO =
                    (zoneRegion.delayVibLFO < -11950.0f
                         ? 0.0f
                         : tsf_timecents2Secsf(zoneRegion.delayVibLFO));

                // Fixup sample positions
                pshdr = &hydra->shdrs[pigen->genAmount.wordAmount];
                zoneRegion.offset += pshdr->start;
                zoneRegion.end += pshdr->end;
                zoneRegion.loop_start += pshdr->startLoop;
                zoneRegion.loop_end += pshdr->endLoop;
                if (pshdr->endLoop > 0)
                  zoneRegion.loop_end -= 1;
                if (zoneRegion.loop_end > fontSampleCount)
                  zoneRegion.loop_end = fontSampleCount;
                if (zoneRegion.pitch_keycenter == -1)
                  zoneRegion.pitch_keycenter = pshdr->originalPitch;
                zoneRegion.tune += pshdr->pitchCorrection;
                zoneRegion.sample_rate = pshdr->sampleRate;
                if (zoneRegion.end && zoneRegion.end < fontSampleCount)
                  zoneRegion.end++;
                else
                  zoneRegion.end = fontSampleCount;

                preset->regions[region_index] = zoneRegion;
                region_index++;
                hadSampleID = 1;
              } else
                tsf_region_operator(&zoneRegion, pigen->genOper,
                                    &pigen->genAmount, TSF_NULL);
            }

            // Handle instrument's global zone.
            if (pibag == hydra->ibags + pinst->instBagNdx && !hadSampleID)
              instRegion = zoneRegion;

            // Modulators (TODO)
            // if (ibag->instModNdx < ibag[1].instModNdx)
            // addUnsupportedOpcode("any modulator");
          }
          hadGenInstrument = 1;
        } else
          tsf_region_operator(&presetRegion, ppgen->genOper, &ppgen->genAmount,
                              TSF_NULL);
      }

      // Modulators (TODO)
      // if (pbag->modNdx < pbag[1].modNdx) addUnsupportedOpcode("any
      // modulator");

      // Handle preset's global zone.
      if (ppbag == hydra->pbags + pphdr->presetBagNdx && !hadGenInstrument)
        globalRegion = presetRegion;
    }

    if (!tsf_preset_build_key_dispatch(preset)) {
      int i;
      for (i = 0; i != res->presetNum; i++) {
        TSF_FREE(res->presets[i].regions);
        tsf_preset_free_key_dispatch(&res->presets[i]);
      }
      TSF_FREE(res->presets);
      return 0;
    }
  }
  return 1;
}

#ifdef STB_VORBIS_INCLUDE_STB_VORBIS_H
static int tsf_decode_ogg(const tsf_u8 *pSmpl, const tsf_u8 *pSmplEnd,
                          float **pRes, tsf_u32 *pResNum, tsf_u32 *pResMax,
                          tsf_u32 resInitial) {
  float *res = *pRes, *oldres;
  tsf_u32 resNum = *pResNum;
  tsf_u32 resMax = *pResMax;
  stb_vorbis *v;

// Use whatever stb_vorbis API that is available (either pull or push)
#if !defined(STB_VORBIS_NO_PULLDATA_API) && !defined(STB_VORBIS_NO_FROMMEMORY)
  v = stb_vorbis_open_memory(pSmpl, (int)(pSmplEnd - pSmpl), TSF_NULL,
                             TSF_NULL);
#else
  {
    int use, err;
    v = stb_vorbis_open_pushdata(pSmpl, (int)(pSmplEnd - pSmpl), &use, &err,
                                 TSF_NULL);
    pSmpl += use;
  }
#endif
  if (v == TSF_NULL)
    return 0;

  for (;;) {
    float **outputs;
    int n_samples;

// Decode one frame of vorbis samples with whatever stb_vorbis API that is
// available
#if !defined(STB_VORBIS_NO_PULLDATA_API) && !defined(STB_VORBIS_NO_FROMMEMORY)
    n_samples = stb_vorbis_get_frame_float(v, TSF_NULL, &outputs);
    if (!n_samples)
      break;
#else
    if (pSmpl >= pSmplEnd)
      break;
    {
      int use = stb_vorbis_decode_frame_pushdata(
          v, pSmpl, (int)(pSmplEnd - pSmpl), TSF_NULL, &outputs, &n_samples);
      pSmpl += use;
    }
    if (!n_samples)
      continue;
#endif

    // Expand our output buffer if necessary then copy over the decoded frame
    // samples
    resNum += n_samples;
    if (resNum > resMax) {
      do {
        resMax += (resMax ? (resMax < 1048576 ? resMax : 1048576) : resInitial);
      } while (resNum > resMax);
      oldres = res;
      res = (float *)TSF_REALLOC(res, resMax * sizeof(float));
      if (!res) {
        TSF_FREE(oldres);
        stb_vorbis_close(v);
        return 0;
      }
    }
    TSF_MEMCPY(res + resNum - n_samples, outputs[0], n_samples * sizeof(float));
  }
  stb_vorbis_close(v);
  *pRes = res;
  *pResNum = resNum;
  *pResMax = resMax;
  return 1;
}

static int tsf_decode_sf3_samples(const void *rawBuffer, float **pFloatBuffer,
                                  unsigned int *pSmplCount,
                                  struct tsf_hydra *hydra) {
  const tsf_u8 *smplBuffer = (const tsf_u8 *)rawBuffer;
  tsf_u32 smplLength = *pSmplCount, resNum = 0, resMax = 0,
          resInitial =
              (smplLength > 0x100000 ? (smplLength & ~0xFFFFF) : 65536);
  float *res = TSF_NULL, *oldres;
  int i, shdrLast = hydra->shdrNum - 1, is_sf3 = 0;
  for (i = 0; i <= shdrLast; i++) {
    struct tsf_hydra_shdr *shdr = &hydra->shdrs[i];
    if (shdr->sampleType & 0x30) // compression flags (sometimes Vorbis flag)
    {
      const tsf_u8 *pSmpl = smplBuffer + shdr->start,
                   *pSmplEnd = smplBuffer + shdr->end;
      if (pSmpl + 4 > pSmplEnd || !TSF_FourCCEquals(pSmpl, "OggS")) {
        shdr->start = shdr->end = shdr->startLoop = shdr->endLoop = 0;
        continue;
      }

      // Fix up sample indices in shdr (end index is set after decoding)
      shdr->start = resNum;
      shdr->startLoop += resNum;
      shdr->endLoop += resNum;
      if (!tsf_decode_ogg(pSmpl, pSmplEnd, &res, &resNum, &resMax,
                          resInitial)) {
        TSF_FREE(res);
        return 0;
      }
      shdr->end = resNum;
      is_sf3 = 1;
    } else // raw PCM sample
    {
      float *out;
      short *in = (short *)smplBuffer + resNum, *inEnd;
      tsf_u32 oldResNum = resNum;
      if (is_sf3) // Fix up sample indices in shdr
      {
        tsf_u32 fix_offset = resNum - shdr->start;
        in -= fix_offset;
        shdr->start = resNum;
        shdr->end += fix_offset;
        shdr->startLoop += fix_offset;
        shdr->endLoop += fix_offset;
      }
      inEnd = in + ((shdr->end >= shdr->endLoop ? shdr->end : shdr->endLoop) -
                    resNum);
      if (i == shdrLast || (tsf_u8 *)inEnd > (smplBuffer + smplLength))
        inEnd = (short *)(smplBuffer + smplLength);
      if (inEnd <= in)
        continue;

      // expand our output buffer if necessary then convert the PCM data from
      // short to float
      resNum += (tsf_u32)(inEnd - in);
      if (resNum > resMax) {
        do {
          resMax +=
              (resMax ? (resMax < 1048576 ? resMax : 1048576) : resInitial);
        } while (resNum > resMax);
        oldres = res;
        res = (float *)TSF_REALLOC(res, resMax * sizeof(float));
        if (!res) {
          TSF_FREE(oldres);
          return 0;
        }
      }

      // Convert the samples from short to float
      for (out = res + oldResNum; in < inEnd;)
        *(out++) = (float)(*(in++) / 32767.0);
    }
  }

  // Trim the sample buffer down then return success (unless out of memory)
  if (!(*pFloatBuffer = (float *)TSF_REALLOC(res, resNum * sizeof(float))))
    *pFloatBuffer = res;
  *pSmplCount = resNum;
  return (res ? 1 : 0);
}
#endif

static int tsf_load_samples(void **pRawBuffer, float **pFloatBuffer,
                            unsigned int *pSmplCount,
                            struct tsf_riffchunk *chunkSmpl,
                            struct tsf_stream *stream) {
#ifdef STB_VORBIS_INCLUDE_STB_VORBIS_H
  // With OGG Vorbis support we cannot pre-allocate the memory for
  // tsf_decode_sf3_samples
  tsf_u32 resNum, resMax;
  float *oldres;
  *pSmplCount = chunkSmpl->size;
  *pRawBuffer = (void *)TSF_MALLOC(*pSmplCount);
  if (!*pRawBuffer || !stream->read(stream->data, *pRawBuffer, chunkSmpl->size))
    return 0;
  if (chunkSmpl->id[3] != 'o')
    return 1;

  // Decode custom .sfo 'smpo' format where all samples are in a single ogg
  // stream
  resNum = resMax = 0;
  if (!tsf_decode_ogg((tsf_u8 *)*pRawBuffer,
                      (tsf_u8 *)*pRawBuffer + chunkSmpl->size, pFloatBuffer,
                      &resNum, &resMax, 65536))
    return 0;
  oldres = *pFloatBuffer;
  if (!(*pFloatBuffer =
            (float *)TSF_REALLOC(*pFloatBuffer, resNum * sizeof(float))))
    *pFloatBuffer = oldres;
  *pSmplCount = resNum;
  return (*pFloatBuffer ? 1 : 0);
#else
  // Inline convert the samples from short to float
  float *res, *out;
  const short *in;
  (void)pRawBuffer;
  *pSmplCount = chunkSmpl->size / (unsigned int)sizeof(short);
  *pFloatBuffer = (float *)TSF_MALLOC(*pSmplCount * sizeof(float));
  if (!*pFloatBuffer ||
      !stream->read(stream->data, *pFloatBuffer, chunkSmpl->size))
    return 0;
  for (res = *pFloatBuffer, out = res + *pSmplCount,
      in = (short *)res + *pSmplCount;
       out != res;)
    *(--out) = (float)(*(--in) / 32767.0);
  return 1;
#endif
}

static int tsf_voice_envelope_release_samples(struct tsf_voice_envelope *e,
                                              float outSampleRate) {
  return (int)((e->parameters.release <= 0 ? TSF_FASTRELEASETIME
                                           : e->parameters.release) *
               outSampleRate);
}

static void tsf_voice_envelope_nextsegment(struct tsf_voice_envelope *e,
                                           short active_segment,
                                           float outSampleRate) {
  switch (active_segment) {
  case TSF_SEGMENT_NONE:
    e->samplesUntilNextSegment = (int)(e->parameters.delay * outSampleRate);
    if (e->samplesUntilNextSegment > 0) {
      e->segment = TSF_SEGMENT_DELAY;
      e->segmentIsExponential = TSF_FALSE;
      e->level = 0.0;
      e->slope = 0.0;
      return;
    }
    /* fall through */
  case TSF_SEGMENT_DELAY:
    e->samplesUntilNextSegment = (int)(e->parameters.attack * outSampleRate);
    if (e->samplesUntilNextSegment > 0) {
      if (!e->isAmpEnv) {
        // mod env attack duration scales with velocity (velocity of 1 is full
        // duration, max velocity is 0.125 times duration)
        e->samplesUntilNextSegment =
            (int)(e->parameters.attack * ((145 - e->midiVelocity) / 144.0f) *
                  outSampleRate);
      }
      e->segment = TSF_SEGMENT_ATTACK;
      e->segmentIsExponential = TSF_FALSE;
      e->level = 0.0f;
      e->slope = 1.0f / e->samplesUntilNextSegment;
      return;
    }
    /* fall through */
  case TSF_SEGMENT_ATTACK:
    e->samplesUntilNextSegment = (int)(e->parameters.hold * outSampleRate);
    if (e->samplesUntilNextSegment > 0) {
      e->segment = TSF_SEGMENT_HOLD;
      e->segmentIsExponential = TSF_FALSE;
      e->level = 1.0f;
      e->slope = 0.0f;
      return;
    }
    /* fall through */
  case TSF_SEGMENT_HOLD:
    e->samplesUntilNextSegment = (int)(e->parameters.decay * outSampleRate);
    if (e->samplesUntilNextSegment > 0) {
      e->segment = TSF_SEGMENT_DECAY;
      e->level = 1.0f;
      if (e->isAmpEnv) {
        // I don't truly understand this; just following what LinuxSampler does.
        float mysterySlope = -9.226f / e->samplesUntilNextSegment;
        e->slope = TSF_EXPF(mysterySlope);
        e->segmentIsExponential = TSF_TRUE;
        if (e->parameters.sustain > 0.0f) {
          // Again, this is following LinuxSampler's example, which is similar
          // to SF2-style decay, where "decay" specifies the time it would take
          // to get to zero, not to the sustain level.  The SFZ spec is not that
          // specific about what "decay" means, so perhaps it's really supposed
          // to specify the time to reach the sustain level.
          e->samplesUntilNextSegment =
              (int)(TSF_LOG(e->parameters.sustain) / mysterySlope);
        }
      } else {
        e->slope = -1.0f / e->samplesUntilNextSegment;
        e->samplesUntilNextSegment =
            (int)(e->parameters.decay * (1.0f - e->parameters.sustain) *
                  outSampleRate);
        e->segmentIsExponential = TSF_FALSE;
      }
      return;
    }
    /* fall through */
  case TSF_SEGMENT_DECAY:
    e->segment = TSF_SEGMENT_SUSTAIN;
    e->level = e->parameters.sustain;
    e->slope = 0.0f;
    e->samplesUntilNextSegment = 0x7FFFFFFF;
    e->segmentIsExponential = TSF_FALSE;
    return;
  case TSF_SEGMENT_SUSTAIN:
    e->segment = TSF_SEGMENT_RELEASE;
    e->samplesUntilNextSegment =
        tsf_voice_envelope_release_samples(e, outSampleRate);
    if (e->isAmpEnv) {
      // I don't truly understand this; just following what LinuxSampler does.
      float mysterySlope = -9.226f / e->samplesUntilNextSegment;
      e->slope = TSF_EXPF(mysterySlope);
      e->segmentIsExponential = TSF_TRUE;
    } else {
      e->slope = -e->level / e->samplesUntilNextSegment;
      e->segmentIsExponential = TSF_FALSE;
    }
    return;
  case TSF_SEGMENT_RELEASE:
  default:
    e->segment = TSF_SEGMENT_DONE;
    e->segmentIsExponential = TSF_FALSE;
    e->level = e->slope = 0.0f;
    e->samplesUntilNextSegment = 0x7FFFFFF;
  }
}

static void tsf_voice_envelope_setup(struct tsf_voice_envelope *e,
                                     struct tsf_envelope *new_parameters,
                                     int midiNoteNumber, short midiVelocity,
                                     TSF_BOOL isAmpEnv, float outSampleRate) {
  e->parameters = *new_parameters;
  if (e->parameters.keynumToHold) {
    e->parameters.hold += e->parameters.keynumToHold * (60.0f - midiNoteNumber);
    e->parameters.hold = (e->parameters.hold < -10000.0f
                              ? 0.0f
                              : tsf_timecents2Secsf(e->parameters.hold));
  }
  if (e->parameters.keynumToDecay) {
    e->parameters.decay +=
        e->parameters.keynumToDecay * (60.0f - midiNoteNumber);
    e->parameters.decay = (e->parameters.decay < -10000.0f
                               ? 0.0f
                               : tsf_timecents2Secsf(e->parameters.decay));
  }
  e->midiVelocity = midiVelocity;
  e->isAmpEnv = isAmpEnv;
  tsf_voice_envelope_nextsegment(e, TSF_SEGMENT_NONE, outSampleRate);
}

static void tsf_voice_envelope_process(struct tsf_voice_envelope *e,
                                       int numSamples, float outSampleRate) {
  if (e->slope) {
    if (e->segmentIsExponential)
      e->level *= TSF_POWF(e->slope, (float)numSamples);
    else
      e->level += (e->slope * numSamples);
  }
  if ((e->samplesUntilNextSegment -= numSamples) <= 0)
    tsf_voice_envelope_nextsegment(e, e->segment, outSampleRate);
}

static void tsf_voice_lowpass_setup(struct tsf_voice_lowpass *e, float Fc) {
  // Lowpass filter from
  // http://www.earlevel.com/main/2012/11/26/biquad-c-source-code/
  double K = TSF_TAN(TSF_PI * Fc), KK = K * K;
  double norm = 1 / (1 + K * e->QInv + KK);
  e->a0 = KK * norm;
  e->a1 = 2 * e->a0;
  e->b1 = 2 * (KK - 1) * norm;
  e->b2 = (1 - K * e->QInv + KK) * norm;
}

static float tsf_voice_lowpass_process(struct tsf_voice_lowpass *e, double In) {
  double Out = In * e->a0 + e->z1;
  e->z1 = In * e->a1 + e->z2 - e->b1 * Out;
  e->z2 = In * e->a0 - e->b2 * Out;
  return (float)Out;
}

static void tsf_voice_lfo_setup(struct tsf_voice_lfo *e, float delay,
                                int freqCents, float outSampleRate) {
  e->samplesUntil = (int)(delay * outSampleRate);
  e->delta = (4.0f * tsf_cents2Hertz((float)freqCents) / outSampleRate);
  e->level = 0;
}

static void tsf_voice_lfo_process(struct tsf_voice_lfo *e, int blockSamples) {
  if (e->samplesUntil > blockSamples) {
    e->samplesUntil -= blockSamples;
    return;
  }
  e->level += e->delta * blockSamples;
  if (e->level > 1.0f) {
    e->delta = -e->delta;
    e->level = 2.0f - e->level;
  } else if (e->level < -1.0f) {
    e->delta = -e->delta;
    e->level = -2.0f - e->level;
  }
}

static void *tsf_aligned_malloc(size_t size, size_t alignment) {
#if defined(_MSC_VER)
  return _aligned_malloc(size, alignment);
#else
  size_t extra = alignment - 1 + sizeof(void *);
  void *raw = TSF_MALLOC(size + extra);
  if (!raw)
    return TSF_NULL;
  {
    uintptr_t aligned = ((uintptr_t)raw + sizeof(void *) + alignment - 1) &
                        ~((uintptr_t)alignment - 1);
    ((void **)aligned)[-1] = raw;
    return (void *)aligned;
  }
#endif
}

static void tsf_aligned_free(void *ptr) {
  if (!ptr)
    return;
#if defined(_MSC_VER)
  _aligned_free(ptr);
#else
    TSF_FREE(((void **)ptr)[-1]);
#endif
}

static void *tsf_aligned_realloc_copy(void *ptr, size_t oldSize, size_t newSize,
                                      size_t alignment) {
  void *res = tsf_aligned_malloc(newSize, alignment);
  if (!res)
    return TSF_NULL;
  if (ptr) {
    TSF_MEMCPY(res, ptr, (oldSize < newSize ? oldSize : newSize));
    tsf_aligned_free(ptr);
  }
  return res;
}

static double tsf_voice_pitch_ratio_cached(const struct tsf_voice *v) {
  return tsf_timecents2Secsd(v->pitchInputTimecents) * v->pitchOutputFactor;
}

static void tsf_refresh_voice_steal_state(tsf *f, int voiceIndex);

static void tsf_voice_filter_hot_from_meta(tsf *f, int voiceIndex) {
  const struct tsf_voice *v = &f->voices[voiceIndex];
  f->voiceFilterZ1Left[voiceIndex] = (float)v->lowpass.z1;
  f->voiceFilterZ2Left[voiceIndex] = (float)v->lowpass.z2;
  f->voiceFilterZ1Right[voiceIndex] = (float)v->lowpass.z1;
  f->voiceFilterZ2Right[voiceIndex] = (float)v->lowpass.z2;
  f->voiceFilterA0[voiceIndex] = (float)v->lowpass.a0;
  f->voiceFilterA1[voiceIndex] = (float)v->lowpass.a1;
  f->voiceFilterB1[voiceIndex] = (float)v->lowpass.b1;
  f->voiceFilterB2[voiceIndex] = (float)v->lowpass.b2;
  f->voiceFilterActive[voiceIndex] = v->lowpass.active ? -1 : 0;
}

static void tsf_voice_update_hot_state(tsf *f, int voiceIndex) {
  struct tsf_voice *v = &f->voices[voiceIndex];
  f->voiceSourceSamplePosition[voiceIndex] = v->sourceSamplePosition;
  f->voiceBasePitchRatio[voiceIndex] = tsf_voice_pitch_ratio_cached(v);
  f->voicePitchRatio[voiceIndex] = f->voiceBasePitchRatio[voiceIndex];
  f->voiceBaseVolumeL[voiceIndex] =
      tsf_decibelsToGain(v->noteGainDB) * v->panFactorLeft;
  f->voiceBaseVolumeR[voiceIndex] =
      tsf_decibelsToGain(v->noteGainDB) * v->panFactorRight;
  f->voiceVolumeL[voiceIndex] = f->voiceBaseVolumeL[voiceIndex];
  f->voiceVolumeR[voiceIndex] = f->voiceBaseVolumeR[voiceIndex];
  f->voiceAmpEnvLevel[voiceIndex] = v->ampenv.level;
  f->voiceAmpEnvSlope[voiceIndex] = v->ampenv.slope;
  f->voiceModEnvLevel[voiceIndex] = v->modenv.level;
  f->voiceModEnvSlope[voiceIndex] = v->modenv.slope;
  f->voiceModLfoLevel[voiceIndex] = v->modlfo.level;
  f->voiceModLfoDelta[voiceIndex] = v->modlfo.delta;
  f->voiceVibLfoLevel[voiceIndex] = v->viblfo.level;
  f->voiceVibLfoDelta[voiceIndex] = v->viblfo.delta;
  f->voiceAmpEnvSamplesUntilNextSegment[voiceIndex] =
      v->ampenv.samplesUntilNextSegment;
  f->voiceModEnvSamplesUntilNextSegment[voiceIndex] =
      v->modenv.samplesUntilNextSegment;
  f->voiceModLfoSamplesUntil[voiceIndex] = v->modlfo.samplesUntil;
  f->voiceVibLfoSamplesUntil[voiceIndex] = v->viblfo.samplesUntil;
  tsf_voice_filter_hot_from_meta(f, voiceIndex);
  if (v->playingPreset == -1 || v->ampenv.segment == TSF_SEGMENT_DONE)
    f->voiceState[voiceIndex] = TSF_VOICE_STATE_OFF;
  else if (v->ampenv.segment >= TSF_SEGMENT_RELEASE)
    f->voiceState[voiceIndex] = TSF_VOICE_STATE_RELEASE;
  else if (v->heldSustain)
    f->voiceState[voiceIndex] = TSF_VOICE_STATE_SUSTAIN;
  else
    f->voiceState[voiceIndex] = TSF_VOICE_STATE_PLAYING;
  tsf_refresh_voice_steal_state(f, voiceIndex);
}

static void tsf_voice_sync_meta_from_hot(tsf *f, int voiceIndex) {
  struct tsf_voice *v = &f->voices[voiceIndex];
  v->sourceSamplePosition = f->voiceSourceSamplePosition[voiceIndex];
  v->ampenv.level = f->voiceAmpEnvLevel[voiceIndex];
  v->ampenv.slope = f->voiceAmpEnvSlope[voiceIndex];
  v->ampenv.samplesUntilNextSegment =
      f->voiceAmpEnvSamplesUntilNextSegment[voiceIndex];
  v->modenv.level = f->voiceModEnvLevel[voiceIndex];
  v->modenv.slope = f->voiceModEnvSlope[voiceIndex];
  v->modenv.samplesUntilNextSegment =
      f->voiceModEnvSamplesUntilNextSegment[voiceIndex];
  v->modlfo.level = f->voiceModLfoLevel[voiceIndex];
  v->modlfo.delta = f->voiceModLfoDelta[voiceIndex];
  v->modlfo.samplesUntil = f->voiceModLfoSamplesUntil[voiceIndex];
  v->viblfo.level = f->voiceVibLfoLevel[voiceIndex];
  v->viblfo.delta = f->voiceVibLfoDelta[voiceIndex];
  v->viblfo.samplesUntil = f->voiceVibLfoSamplesUntil[voiceIndex];
  v->lowpass.z1 = f->voiceFilterZ1Left[voiceIndex];
  v->lowpass.z2 = f->voiceFilterZ2Left[voiceIndex];
  v->lowpass.a0 = f->voiceFilterA0[voiceIndex];
  v->lowpass.a1 = f->voiceFilterA1[voiceIndex];
  v->lowpass.b1 = f->voiceFilterB1[voiceIndex];
  v->lowpass.b2 = f->voiceFilterB2[voiceIndex];
  v->lowpass.active = (TSF_BOOL)(f->voiceFilterActive[voiceIndex] != 0);
}

static void tsf_voice_zero_hot_state(tsf *f, int voiceIndex) {
  f->voiceSourceSamplePosition[voiceIndex] = 0.0;
  f->voiceBasePitchRatio[voiceIndex] = 0.0;
  f->voicePitchRatio[voiceIndex] = 0.0;
  f->voiceBaseVolumeL[voiceIndex] = 0.0f;
  f->voiceBaseVolumeR[voiceIndex] = 0.0f;
  f->voiceVolumeL[voiceIndex] = 0.0f;
  f->voiceVolumeR[voiceIndex] = 0.0f;
  f->voiceAmpEnvLevel[voiceIndex] = 0.0f;
  f->voiceAmpEnvSlope[voiceIndex] = 0.0f;
  f->voiceModEnvLevel[voiceIndex] = 0.0f;
  f->voiceModEnvSlope[voiceIndex] = 0.0f;
  f->voiceModLfoLevel[voiceIndex] = 0.0f;
  f->voiceModLfoDelta[voiceIndex] = 0.0f;
  f->voiceVibLfoLevel[voiceIndex] = 0.0f;
  f->voiceVibLfoDelta[voiceIndex] = 0.0f;
  f->voiceFilterZ1Left[voiceIndex] = 0.0f;
  f->voiceFilterZ2Left[voiceIndex] = 0.0f;
  f->voiceFilterZ1Right[voiceIndex] = 0.0f;
  f->voiceFilterZ2Right[voiceIndex] = 0.0f;
  f->voiceFilterA0[voiceIndex] = 0.0f;
  f->voiceFilterA1[voiceIndex] = 0.0f;
  f->voiceFilterB1[voiceIndex] = 0.0f;
  f->voiceFilterB2[voiceIndex] = 0.0f;
  f->voiceAmpEnvSamplesUntilNextSegment[voiceIndex] = 0;
  f->voiceModEnvSamplesUntilNextSegment[voiceIndex] = 0;
  f->voiceModLfoSamplesUntil[voiceIndex] = 0;
  f->voiceVibLfoSamplesUntil[voiceIndex] = 0;
  f->voiceFilterActive[voiceIndex] = 0;
  f->voiceState[voiceIndex] = TSF_VOICE_STATE_OFF;
}

static int tsf_voice_index(const tsf *f, const struct tsf_voice *v) {
  return (int)(v - f->voices);
}

static void tsf_link_voice_to_channel(tsf *f, int voiceIndex);
static void tsf_refresh_voice_steal_state(tsf *f, int voiceIndex);
static void tsf_clear_exclusive_group_cache(tsf *f);
static int tsf_link_voice_to_exclusive_group(tsf *f, int voiceIndex);

static void tsf_clear_voice_steal_cache(tsf *f) {
  int i;
  f->stealActiveLoudHead = f->stealActiveLoudTail = -1;
  f->stealActiveQuietHead = f->stealActiveQuietTail = -1;
  f->stealReleaseLoudHead = f->stealReleaseLoudTail = -1;
  f->stealReleaseQuietHead = f->stealReleaseQuietTail = -1;
  for (i = 0; i < f->voiceNum; ++i) {
    f->voices[i].nextStealVoice = -1;
    f->voices[i].prevStealVoice = -1;
    f->voices[i].stealClass = TSF_STEAL_CLASS_NONE;
  }
}

static unsigned long long tsf_make_exclusive_group_key(int presetIndex,
                                                       unsigned int group) {
  if (presetIndex < 0 || !group)
    return TSF_EXCLUSIVE_GROUP_KEY_EMPTY;
  return (((unsigned long long)(unsigned int)(presetIndex + 1)) << 32) |
         (unsigned long long)group;
}

static unsigned long long
tsf_voice_exclusive_group_key(const struct tsf_voice *v) {
  if (!v || !v->region)
    return TSF_EXCLUSIVE_GROUP_KEY_EMPTY;
  return tsf_make_exclusive_group_key(v->playingPreset, v->region->group);
}

static unsigned int tsf_exclusive_group_hash(unsigned long long key) {
  key ^= key >> 33;
  key *= 0xff51afd7ed558ccdull;
  key ^= key >> 33;
  key *= 0xc4ceb9fe1a85ec53ull;
  key ^= key >> 33;
  return (unsigned int)key;
}

static void tsf_clear_exclusive_group_cache(tsf *f) {
  unsigned int slot;
  int i;
  if (f->exclusiveGroupKeys && f->exclusiveGroupHeads &&
      f->exclusiveGroupCapacity) {
    for (slot = 0; slot < f->exclusiveGroupCapacity; ++slot) {
      f->exclusiveGroupKeys[slot] = TSF_EXCLUSIVE_GROUP_KEY_EMPTY;
      f->exclusiveGroupHeads[slot] = -1;
    }
  }
  f->exclusiveGroupCount = 0;
  f->exclusiveGroupTombstones = 0;
  for (i = 0; i < f->voiceNum; ++i) {
    f->voices[i].nextExclusiveGroupVoice = -1;
    f->voices[i].prevExclusiveGroupVoice = -1;
    f->voices[i].exclusiveGroupSlot = -1;
  }
}

static void tsf_clear_channel_voice_cache(tsf *f) {
  int i, key;
  for (i = 0; i < 16; ++i) {
    f->channelVoiceHeads[i] = -1;
    f->channelVoiceCounts[i] = 0;
    for (key = 0; key < 128; ++key) {
      f->channelKeyVoiceHeads[i][key] = -1;
      f->channelKeyVoiceCounts[i][key] = 0;
    }
  }
  tsf_clear_exclusive_group_cache(f);
  tsf_clear_voice_steal_cache(f);
  f->channelVoiceCacheDirty = 0;
}

static int tsf_exclusive_group_find_slot(const tsf *f,
                                         unsigned long long key) {
  unsigned int slot;
  unsigned int firstTombstone = (unsigned int)-1;
  if (!f->exclusiveGroupKeys || !f->exclusiveGroupCapacity ||
      key <= TSF_EXCLUSIVE_GROUP_KEY_TOMBSTONE)
    return -1;
  slot = tsf_exclusive_group_hash(key) & (f->exclusiveGroupCapacity - 1u);
  while (1) {
    unsigned long long storedKey = f->exclusiveGroupKeys[slot];
    if (storedKey == TSF_EXCLUSIVE_GROUP_KEY_EMPTY)
      return (firstTombstone != (unsigned int)-1 ? (int)firstTombstone
                                                 : (int)slot);
    if (storedKey == key)
      return (int)slot;
    if (storedKey == TSF_EXCLUSIVE_GROUP_KEY_TOMBSTONE &&
        firstTombstone == (unsigned int)-1)
      firstTombstone = slot;
    slot = (slot + 1u) & (f->exclusiveGroupCapacity - 1u);
  }
}

static int tsf_exclusive_group_lookup_slot(const tsf *f,
                                           unsigned long long key) {
  unsigned int slot;
  if (!f->exclusiveGroupKeys || !f->exclusiveGroupCapacity ||
      key <= TSF_EXCLUSIVE_GROUP_KEY_TOMBSTONE)
    return -1;
  slot = tsf_exclusive_group_hash(key) & (f->exclusiveGroupCapacity - 1u);
  while (1) {
    unsigned long long storedKey = f->exclusiveGroupKeys[slot];
    if (storedKey == TSF_EXCLUSIVE_GROUP_KEY_EMPTY)
      return -1;
    if (storedKey == key)
      return (int)slot;
    slot = (slot + 1u) & (f->exclusiveGroupCapacity - 1u);
  }
}

static void tsf_insert_voice_into_exclusive_group_slot(tsf *f, int voiceIndex,
                                                       int slot,
                                                       unsigned long long key) {
  struct tsf_voice *v = &f->voices[voiceIndex];
  if (slot < 0)
    return;
  if (f->exclusiveGroupKeys[slot] == TSF_EXCLUSIVE_GROUP_KEY_EMPTY)
    f->exclusiveGroupCount++;
  else if (f->exclusiveGroupKeys[slot] == TSF_EXCLUSIVE_GROUP_KEY_TOMBSTONE) {
    if (f->exclusiveGroupTombstones > 0)
      f->exclusiveGroupTombstones--;
    f->exclusiveGroupCount++;
  }
  if (f->exclusiveGroupKeys[slot] != key) {
    f->exclusiveGroupKeys[slot] = key;
    f->exclusiveGroupHeads[slot] = -1;
  }
  v->exclusiveGroupSlot = slot;
  v->prevExclusiveGroupVoice = -1;
  v->nextExclusiveGroupVoice = f->exclusiveGroupHeads[slot];
  if (f->exclusiveGroupHeads[slot] != -1)
    f->voices[f->exclusiveGroupHeads[slot]].prevExclusiveGroupVoice = voiceIndex;
  f->exclusiveGroupHeads[slot] = voiceIndex;
}

static void tsf_unlink_voice_from_exclusive_group(tsf *f, int voiceIndex) {
  struct tsf_voice *v = &f->voices[voiceIndex];
  int slot = v->exclusiveGroupSlot;
  if (slot < 0 || !f->exclusiveGroupHeads || !f->exclusiveGroupKeys ||
      (unsigned int)slot >= f->exclusiveGroupCapacity) {
    v->nextExclusiveGroupVoice = -1;
    v->prevExclusiveGroupVoice = -1;
    v->exclusiveGroupSlot = -1;
    return;
  }

  if (v->prevExclusiveGroupVoice != -1)
    f->voices[v->prevExclusiveGroupVoice].nextExclusiveGroupVoice =
        v->nextExclusiveGroupVoice;
  else if (f->exclusiveGroupHeads[slot] == voiceIndex)
    f->exclusiveGroupHeads[slot] = v->nextExclusiveGroupVoice;

  if (v->nextExclusiveGroupVoice != -1)
    f->voices[v->nextExclusiveGroupVoice].prevExclusiveGroupVoice =
        v->prevExclusiveGroupVoice;

  if (f->exclusiveGroupHeads[slot] == -1 &&
      f->exclusiveGroupKeys[slot] > TSF_EXCLUSIVE_GROUP_KEY_TOMBSTONE) {
    f->exclusiveGroupKeys[slot] = TSF_EXCLUSIVE_GROUP_KEY_TOMBSTONE;
    if (f->exclusiveGroupCount > 0)
      f->exclusiveGroupCount--;
    f->exclusiveGroupTombstones++;
  }

  v->nextExclusiveGroupVoice = -1;
  v->prevExclusiveGroupVoice = -1;
  v->exclusiveGroupSlot = -1;
}

static int tsf_ensure_exclusive_group_cache_capacity(tsf *f,
                                                     unsigned int minEntries) {
  unsigned int desiredCapacity = 16;
  unsigned long long *newKeys;
  int *newHeads;
  unsigned long long *oldKeys = f->exclusiveGroupKeys;
  int *oldHeads = f->exclusiveGroupHeads;
  unsigned int oldCapacity = f->exclusiveGroupCapacity;
  unsigned int i;

  if (minEntries < 8)
    minEntries = 8;
  while (desiredCapacity < minEntries * 2u)
    desiredCapacity <<= 1u;

  if (f->exclusiveGroupCapacity >= desiredCapacity &&
      f->exclusiveGroupTombstones <=
          (f->exclusiveGroupCapacity >> 2))
    return 1;

  newKeys = (unsigned long long *)TSF_MALLOC(desiredCapacity *
                                             sizeof(unsigned long long));
  if (!newKeys)
    return 0;
  newHeads = (int *)TSF_MALLOC(desiredCapacity * sizeof(int));
  if (!newHeads) {
    TSF_FREE(newKeys);
    return 0;
  }
  for (i = 0; i < desiredCapacity; ++i) {
    newKeys[i] = TSF_EXCLUSIVE_GROUP_KEY_EMPTY;
    newHeads[i] = -1;
  }

  f->exclusiveGroupKeys = newKeys;
  f->exclusiveGroupHeads = newHeads;
  f->exclusiveGroupCapacity = desiredCapacity;
  f->exclusiveGroupCount = 0;
  f->exclusiveGroupTombstones = 0;
  for (i = 0; i < (unsigned int)f->voiceNum; ++i) {
    f->voices[i].nextExclusiveGroupVoice = -1;
    f->voices[i].prevExclusiveGroupVoice = -1;
    f->voices[i].exclusiveGroupSlot = -1;
  }
  for (i = 0; i < (unsigned int)f->activeVoiceCount; ++i) {
    int voiceIndex = f->activeVoiceIndices[i];
    struct tsf_voice *v = &f->voices[voiceIndex];
    unsigned long long key = tsf_voice_exclusive_group_key(v);
    int slot;
    if (v->playingPreset == -1 || key == TSF_EXCLUSIVE_GROUP_KEY_EMPTY)
      continue;
    slot = tsf_exclusive_group_find_slot(f, key);
    tsf_insert_voice_into_exclusive_group_slot(f, voiceIndex, slot, key);
  }

  if (oldKeys)
    TSF_FREE(oldKeys);
  if (oldHeads)
    TSF_FREE(oldHeads);
  return 1;
}

static int tsf_link_voice_to_exclusive_group(tsf *f, int voiceIndex) {
  struct tsf_voice *v = &f->voices[voiceIndex];
  unsigned long long key = tsf_voice_exclusive_group_key(v);
  int slot;
  if (key == TSF_EXCLUSIVE_GROUP_KEY_EMPTY) {
    v->nextExclusiveGroupVoice = -1;
    v->prevExclusiveGroupVoice = -1;
    v->exclusiveGroupSlot = -1;
    return 1;
  }
  if (!tsf_ensure_exclusive_group_cache_capacity(f, (unsigned int)f->activeVoiceCount + 1u))
    return 0;
  slot = tsf_exclusive_group_find_slot(f, key);
  if (slot < 0)
    return 0;
  tsf_insert_voice_into_exclusive_group_slot(f, voiceIndex, slot, key);
  return 1;
}

static void tsf_rebuild_channel_voice_cache(tsf *f) {
  int i;
  tsf_clear_channel_voice_cache(f);
  for (i = 0; i < f->activeVoiceCount; ++i) {
    int voiceIndex = f->activeVoiceIndices[i];
    struct tsf_voice *v = &f->voices[voiceIndex];
    if (v->playingPreset != -1) {
      tsf_link_voice_to_channel(f, voiceIndex);
      tsf_link_voice_to_exclusive_group(f, voiceIndex);
      tsf_refresh_voice_steal_state(f, voiceIndex);
    }
  }
  f->channelVoiceCacheDirty = 0;
}

static void tsf_ensure_channel_voice_cache(tsf *f) {
  if (f->channelVoiceCacheDirty)
    tsf_rebuild_channel_voice_cache(f);
}

static void tsf_link_voice_to_channel(tsf *f, int voiceIndex) {
  struct tsf_voice *v = &f->voices[voiceIndex];
  if (v->playingChannel < 0 || v->playingChannel >= 16) {
    v->nextChannelVoice = -1;
    v->nextChannelKeyVoice = -1;
    return;
  }

  v->nextChannelVoice = f->channelVoiceHeads[v->playingChannel];
  f->channelVoiceHeads[v->playingChannel] = voiceIndex;
  f->channelVoiceCounts[v->playingChannel]++;
  if (v->playingKey >= 0 && v->playingKey < 128) {
    v->nextChannelKeyVoice =
        f->channelKeyVoiceHeads[v->playingChannel][v->playingKey];
    f->channelKeyVoiceHeads[v->playingChannel][v->playingKey] = voiceIndex;
    f->channelKeyVoiceCounts[v->playingChannel][v->playingKey]++;
  } else {
    v->nextChannelKeyVoice = -1;
  }
}

static TSF_BOOL tsf_voice_is_quiet_candidate(const struct tsf_voice *v) {
  return (TSF_BOOL)(v && v->midiVelocity <= TSF_STEAL_QUIET_VELOCITY_MAX);
}

static int tsf_voice_steal_class(const struct tsf_voice *v) {
  if (!v || v->playingPreset == -1 || v->ampenv.segment == TSF_SEGMENT_DONE)
    return TSF_STEAL_CLASS_NONE;
  if (v->ampenv.segment >= TSF_SEGMENT_RELEASE)
    return tsf_voice_is_quiet_candidate(v) ? TSF_STEAL_CLASS_RELEASE_QUIET
                                           : TSF_STEAL_CLASS_RELEASE_LOUD;
  return tsf_voice_is_quiet_candidate(v) ? TSF_STEAL_CLASS_ACTIVE_QUIET
                                         : TSF_STEAL_CLASS_ACTIVE_LOUD;
}

static void tsf_unlink_voice_from_steal_list(tsf *f, int voiceIndex) {
  struct tsf_voice *v = &f->voices[voiceIndex];
  int *head;
  int *tail;
  if (v->stealClass == TSF_STEAL_CLASS_ACTIVE_LOUD) {
    head = &f->stealActiveLoudHead;
    tail = &f->stealActiveLoudTail;
  } else if (v->stealClass == TSF_STEAL_CLASS_ACTIVE_QUIET) {
    head = &f->stealActiveQuietHead;
    tail = &f->stealActiveQuietTail;
  } else if (v->stealClass == TSF_STEAL_CLASS_RELEASE_LOUD) {
    head = &f->stealReleaseLoudHead;
    tail = &f->stealReleaseLoudTail;
  } else if (v->stealClass == TSF_STEAL_CLASS_RELEASE_QUIET) {
    head = &f->stealReleaseQuietHead;
    tail = &f->stealReleaseQuietTail;
  } else {
    v->nextStealVoice = -1;
    v->prevStealVoice = -1;
    return;
  }

  if (v->prevStealVoice != -1)
    f->voices[v->prevStealVoice].nextStealVoice = v->nextStealVoice;
  else
    *head = v->nextStealVoice;

  if (v->nextStealVoice != -1)
    f->voices[v->nextStealVoice].prevStealVoice = v->prevStealVoice;
  else
    *tail = v->prevStealVoice;

  v->nextStealVoice = -1;
  v->prevStealVoice = -1;
  v->stealClass = TSF_STEAL_CLASS_NONE;
}

static void tsf_link_voice_to_steal_list(tsf *f, int voiceIndex, int stealClass) {
  struct tsf_voice *v = &f->voices[voiceIndex];
  int *head;
  int *tail;
  if (stealClass == TSF_STEAL_CLASS_ACTIVE_LOUD) {
    head = &f->stealActiveLoudHead;
    tail = &f->stealActiveLoudTail;
  } else if (stealClass == TSF_STEAL_CLASS_ACTIVE_QUIET) {
    head = &f->stealActiveQuietHead;
    tail = &f->stealActiveQuietTail;
  } else if (stealClass == TSF_STEAL_CLASS_RELEASE_LOUD) {
    head = &f->stealReleaseLoudHead;
    tail = &f->stealReleaseLoudTail;
  } else if (stealClass == TSF_STEAL_CLASS_RELEASE_QUIET) {
    head = &f->stealReleaseQuietHead;
    tail = &f->stealReleaseQuietTail;
  } else {
    v->stealClass = TSF_STEAL_CLASS_NONE;
    v->nextStealVoice = -1;
    v->prevStealVoice = -1;
    return;
  }

  v->prevStealVoice = *tail;
  v->nextStealVoice = -1;
  v->stealClass = stealClass;
  if (*tail != -1)
    f->voices[*tail].nextStealVoice = voiceIndex;
  else
    *head = voiceIndex;
  *tail = voiceIndex;
}

static void tsf_refresh_voice_steal_state(tsf *f, int voiceIndex) {
  struct tsf_voice *v = &f->voices[voiceIndex];
  int newClass = tsf_voice_steal_class(v);
  if (v->stealClass == newClass)
    return;
  if (v->stealClass != TSF_STEAL_CLASS_NONE)
    tsf_unlink_voice_from_steal_list(f, voiceIndex);
  if (newClass != TSF_STEAL_CLASS_NONE)
    tsf_link_voice_to_steal_list(f, voiceIndex, newClass);
}

static int tsf_reserve_voice_storage(tsf *f, int newVoiceNum) {
  struct tsf_voice *newVoices;
  double *newSourceSamplePosition;
  double *newBasePitchRatio;
  double *newPitchRatio;
  float *newBaseVolumeL;
  float *newBaseVolumeR;
  float *newVolumeL;
  float *newVolumeR;
  float *newAmpEnvLevel;
  float *newAmpEnvSlope;
  float *newModEnvLevel;
  float *newModEnvSlope;
  float *newModLfoLevel;
  float *newModLfoDelta;
  float *newVibLfoLevel;
  float *newVibLfoDelta;
  float *newFilterZ1Left;
  float *newFilterZ2Left;
  float *newFilterZ1Right;
  float *newFilterZ2Right;
  float *newFilterA0;
  float *newFilterA1;
  float *newFilterB1;
  float *newFilterB2;
  int *newAmpEnvSamplesUntilNextSegment;
  int *newModEnvSamplesUntilNextSegment;
  int *newModLfoSamplesUntil;
  int *newVibLfoSamplesUntil;
  int *newFilterActive;
  int *newVoiceState;
  int *newActiveVoiceIndices;
  int *newFreeVoiceIndices;
  size_t oldFloatBytes, newFloatBytes, oldDoubleBytes, newDoubleBytes,
      oldIntBytes, newIntBytes;

  if (newVoiceNum <= f->voiceNum)
    return 1;

  oldFloatBytes = (size_t)f->voiceNum * sizeof(float);
  newFloatBytes = (size_t)newVoiceNum * sizeof(float);
  oldDoubleBytes = (size_t)f->voiceNum * sizeof(double);
  newDoubleBytes = (size_t)newVoiceNum * sizeof(double);
  oldIntBytes = (size_t)f->voiceNum * sizeof(int);
  newIntBytes = (size_t)newVoiceNum * sizeof(int);

  newVoices = (struct tsf_voice *)TSF_REALLOC(
      f->voices, newVoiceNum * sizeof(struct tsf_voice));
  if (!newVoices)
    return 0;
  f->voices = newVoices;

  newSourceSamplePosition = (double *)tsf_aligned_realloc_copy(
      f->voiceSourceSamplePosition, oldDoubleBytes, newDoubleBytes, 32);
  if (!newSourceSamplePosition)
    return 0;
  f->voiceSourceSamplePosition = newSourceSamplePosition;

  newBasePitchRatio = (double *)tsf_aligned_realloc_copy(
      f->voiceBasePitchRatio, oldDoubleBytes, newDoubleBytes, 32);
  if (!newBasePitchRatio)
    return 0;
  f->voiceBasePitchRatio = newBasePitchRatio;

  newPitchRatio = (double *)tsf_aligned_realloc_copy(
      f->voicePitchRatio, oldDoubleBytes, newDoubleBytes, 32);
  if (!newPitchRatio)
    return 0;
  f->voicePitchRatio = newPitchRatio;

  newBaseVolumeL = (float *)tsf_aligned_realloc_copy(
      f->voiceBaseVolumeL, oldFloatBytes, newFloatBytes, 32);
  if (!newBaseVolumeL)
    return 0;
  f->voiceBaseVolumeL = newBaseVolumeL;

  newBaseVolumeR = (float *)tsf_aligned_realloc_copy(
      f->voiceBaseVolumeR, oldFloatBytes, newFloatBytes, 32);
  if (!newBaseVolumeR)
    return 0;
  f->voiceBaseVolumeR = newBaseVolumeR;

  newVolumeL = (float *)tsf_aligned_realloc_copy(f->voiceVolumeL, oldFloatBytes,
                                                 newFloatBytes, 32);
  if (!newVolumeL)
    return 0;
  f->voiceVolumeL = newVolumeL;

  newVolumeR = (float *)tsf_aligned_realloc_copy(f->voiceVolumeR, oldFloatBytes,
                                                 newFloatBytes, 32);
  if (!newVolumeR)
    return 0;
  f->voiceVolumeR = newVolumeR;

  newAmpEnvLevel = (float *)tsf_aligned_realloc_copy(
      f->voiceAmpEnvLevel, oldFloatBytes, newFloatBytes, 32);
  if (!newAmpEnvLevel)
    return 0;
  f->voiceAmpEnvLevel = newAmpEnvLevel;

  newAmpEnvSlope = (float *)tsf_aligned_realloc_copy(
      f->voiceAmpEnvSlope, oldFloatBytes, newFloatBytes, 32);
  if (!newAmpEnvSlope)
    return 0;
  f->voiceAmpEnvSlope = newAmpEnvSlope;

  newModEnvLevel = (float *)tsf_aligned_realloc_copy(
      f->voiceModEnvLevel, oldFloatBytes, newFloatBytes, 32);
  if (!newModEnvLevel)
    return 0;
  f->voiceModEnvLevel = newModEnvLevel;

  newModEnvSlope = (float *)tsf_aligned_realloc_copy(
      f->voiceModEnvSlope, oldFloatBytes, newFloatBytes, 32);
  if (!newModEnvSlope)
    return 0;
  f->voiceModEnvSlope = newModEnvSlope;

  newModLfoLevel = (float *)tsf_aligned_realloc_copy(
      f->voiceModLfoLevel, oldFloatBytes, newFloatBytes, 32);
  if (!newModLfoLevel)
    return 0;
  f->voiceModLfoLevel = newModLfoLevel;

  newModLfoDelta = (float *)tsf_aligned_realloc_copy(
      f->voiceModLfoDelta, oldFloatBytes, newFloatBytes, 32);
  if (!newModLfoDelta)
    return 0;
  f->voiceModLfoDelta = newModLfoDelta;

  newVibLfoLevel = (float *)tsf_aligned_realloc_copy(
      f->voiceVibLfoLevel, oldFloatBytes, newFloatBytes, 32);
  if (!newVibLfoLevel)
    return 0;
  f->voiceVibLfoLevel = newVibLfoLevel;

  newVibLfoDelta = (float *)tsf_aligned_realloc_copy(
      f->voiceVibLfoDelta, oldFloatBytes, newFloatBytes, 32);
  if (!newVibLfoDelta)
    return 0;
  f->voiceVibLfoDelta = newVibLfoDelta;

  newFilterZ1Left = (float *)tsf_aligned_realloc_copy(
      f->voiceFilterZ1Left, oldFloatBytes, newFloatBytes, 32);
  if (!newFilterZ1Left)
    return 0;
  f->voiceFilterZ1Left = newFilterZ1Left;

  newFilterZ2Left = (float *)tsf_aligned_realloc_copy(
      f->voiceFilterZ2Left, oldFloatBytes, newFloatBytes, 32);
  if (!newFilterZ2Left)
    return 0;
  f->voiceFilterZ2Left = newFilterZ2Left;

  newFilterZ1Right = (float *)tsf_aligned_realloc_copy(
      f->voiceFilterZ1Right, oldFloatBytes, newFloatBytes, 32);
  if (!newFilterZ1Right)
    return 0;
  f->voiceFilterZ1Right = newFilterZ1Right;

  newFilterZ2Right = (float *)tsf_aligned_realloc_copy(
      f->voiceFilterZ2Right, oldFloatBytes, newFloatBytes, 32);
  if (!newFilterZ2Right)
    return 0;
  f->voiceFilterZ2Right = newFilterZ2Right;

  newFilterA0 = (float *)tsf_aligned_realloc_copy(
      f->voiceFilterA0, oldFloatBytes, newFloatBytes, 32);
  if (!newFilterA0)
    return 0;
  f->voiceFilterA0 = newFilterA0;

  newFilterA1 = (float *)tsf_aligned_realloc_copy(
      f->voiceFilterA1, oldFloatBytes, newFloatBytes, 32);
  if (!newFilterA1)
    return 0;
  f->voiceFilterA1 = newFilterA1;

  newFilterB1 = (float *)tsf_aligned_realloc_copy(
      f->voiceFilterB1, oldFloatBytes, newFloatBytes, 32);
  if (!newFilterB1)
    return 0;
  f->voiceFilterB1 = newFilterB1;

  newFilterB2 = (float *)tsf_aligned_realloc_copy(
      f->voiceFilterB2, oldFloatBytes, newFloatBytes, 32);
  if (!newFilterB2)
    return 0;
  f->voiceFilterB2 = newFilterB2;

  newAmpEnvSamplesUntilNextSegment = (int *)tsf_aligned_realloc_copy(
      f->voiceAmpEnvSamplesUntilNextSegment, oldIntBytes, newIntBytes, 16);
  if (!newAmpEnvSamplesUntilNextSegment)
    return 0;
  f->voiceAmpEnvSamplesUntilNextSegment = newAmpEnvSamplesUntilNextSegment;

  newModEnvSamplesUntilNextSegment = (int *)tsf_aligned_realloc_copy(
      f->voiceModEnvSamplesUntilNextSegment, oldIntBytes, newIntBytes, 16);
  if (!newModEnvSamplesUntilNextSegment)
    return 0;
  f->voiceModEnvSamplesUntilNextSegment = newModEnvSamplesUntilNextSegment;

  newModLfoSamplesUntil = (int *)tsf_aligned_realloc_copy(
      f->voiceModLfoSamplesUntil, oldIntBytes, newIntBytes, 16);
  if (!newModLfoSamplesUntil)
    return 0;
  f->voiceModLfoSamplesUntil = newModLfoSamplesUntil;

  newVibLfoSamplesUntil = (int *)tsf_aligned_realloc_copy(
      f->voiceVibLfoSamplesUntil, oldIntBytes, newIntBytes, 16);
  if (!newVibLfoSamplesUntil)
    return 0;
  f->voiceVibLfoSamplesUntil = newVibLfoSamplesUntil;

  newFilterActive = (int *)tsf_aligned_realloc_copy(
      f->voiceFilterActive, oldIntBytes, newIntBytes, 16);
  if (!newFilterActive)
    return 0;
  f->voiceFilterActive = newFilterActive;

  newVoiceState = (int *)tsf_aligned_realloc_copy(f->voiceState, oldIntBytes,
                                                  newIntBytes, 16);
  if (!newVoiceState)
    return 0;
  f->voiceState = newVoiceState;

  newActiveVoiceIndices = (int *)TSF_REALLOC(
      f->activeVoiceIndices, newVoiceNum * sizeof(int));
  if (!newActiveVoiceIndices)
    return 0;
  f->activeVoiceIndices = newActiveVoiceIndices;

  newFreeVoiceIndices =
      (int *)TSF_REALLOC(f->freeVoiceIndices, newVoiceNum * sizeof(int));
  if (!newFreeVoiceIndices)
    return 0;
  f->freeVoiceIndices = newFreeVoiceIndices;

  for (; f->voiceNum < newVoiceNum; f->voiceNum++) {
    f->voices[f->voiceNum].playingPreset = -1;
    f->voices[f->voiceNum].pendingFree = 0;
    f->voices[f->voiceNum].nextChannelVoice = -1;
    f->voices[f->voiceNum].nextChannelKeyVoice = -1;
    f->voices[f->voiceNum].nextExclusiveGroupVoice = -1;
    f->voices[f->voiceNum].prevExclusiveGroupVoice = -1;
    f->voices[f->voiceNum].exclusiveGroupSlot = -1;
    f->voices[f->voiceNum].nextStealVoice = -1;
    f->voices[f->voiceNum].prevStealVoice = -1;
    f->voices[f->voiceNum].stealClass = TSF_STEAL_CLASS_NONE;
    f->voices[f->voiceNum].playingChannel = -1;
    f->voices[f->voiceNum].heldSustain = 0;
    f->voices[f->voiceNum].region = TSF_NULL;
    f->voices[f->voiceNum].playIndex = 0;
    f->voices[f->voiceNum].midiVelocity = 0;
    tsf_voice_zero_hot_state(f, f->voiceNum);
    f->freeVoiceIndices[f->freeVoiceCount++] = f->voiceNum;
  }
  return 1;
}

static void tsf_activate_voice_index(tsf *f, int voiceIndex) {
  f->activeVoiceIndices[f->activeVoiceCount++] = voiceIndex;
  f->voices[voiceIndex].pendingFree = 0;
  f->voices[voiceIndex].nextChannelVoice = -1;
  f->voices[voiceIndex].nextChannelKeyVoice = -1;
  f->voices[voiceIndex].nextExclusiveGroupVoice = -1;
  f->voices[voiceIndex].prevExclusiveGroupVoice = -1;
  f->voices[voiceIndex].exclusiveGroupSlot = -1;
  f->voices[voiceIndex].nextStealVoice = -1;
  f->voices[voiceIndex].prevStealVoice = -1;
  f->voices[voiceIndex].stealClass = TSF_STEAL_CLASS_NONE;
  f->voiceState[voiceIndex] = TSF_VOICE_STATE_PLAYING;
}

static int tsf_acquire_free_voice_index(tsf *f) {
  if (f->freeVoiceCount > 0)
    return f->freeVoiceIndices[--f->freeVoiceCount];
  return -1;
}

static void tsf_voice_kill(tsf *f, struct tsf_voice *v) {
  int voiceIndex = tsf_voice_index(f, v);
  tsf_unlink_voice_from_exclusive_group(f, voiceIndex);
  tsf_unlink_voice_from_steal_list(f, voiceIndex);
  v->playingPreset = -1;
  v->pendingFree = 1;
  v->nextChannelVoice = -1;
  v->nextChannelKeyVoice = -1;
  tsf_voice_zero_hot_state(f, voiceIndex);
  f->channelVoiceCacheDirty = 1;
}

static float tsf_voice_estimated_gain(const struct tsf_voice *v) {
  float envelopeLevel = (v->ampenv.level > 0.0f ? v->ampenv.level : 0.0f);
  return tsf_decibelsToGain(v->noteGainDB) * envelopeLevel;
}

static struct tsf_voice *tsf_select_voice_to_steal(tsf *f) {
  int voiceIndex = -1;
  if (f->stealReleaseQuietHead != -1)
    voiceIndex = f->stealReleaseQuietHead;
  else if (f->stealActiveQuietHead != -1)
    voiceIndex = f->stealActiveQuietHead;
  else if (f->stealReleaseLoudHead != -1)
    voiceIndex = f->stealReleaseLoudHead;
  else if (f->stealActiveLoudHead != -1)
    voiceIndex = f->stealActiveLoudHead;
  if (voiceIndex == -1)
    return TSF_NULL;
  return &f->voices[voiceIndex];
}

static void tsf_voice_end(tsf *f, struct tsf_voice *v) {
  int voiceIndex = tsf_voice_index(f, v);
  // if maxVoiceNum is set, assume that voice rendering and note queuing are on
  // separate threads so to minimize the chance that voice rendering would
  // advance the segment at the same time we just do it twice here and hope that
  // it sticks
  int repeats = (f->maxVoiceNum ? 2 : 1);
  while (repeats--) {
    tsf_voice_envelope_nextsegment(&v->ampenv, TSF_SEGMENT_SUSTAIN,
                                   f->outSampleRate);
    tsf_voice_envelope_nextsegment(&v->modenv, TSF_SEGMENT_SUSTAIN,
                                   f->outSampleRate);
    if (v->region->loop_mode == TSF_LOOPMODE_SUSTAIN) {
      // Continue playing, but stop looping.
      v->loopEnd = v->loopStart;
    }
  }
  tsf_voice_update_hot_state(f, voiceIndex);
}

static void tsf_voice_endquick(tsf *f, struct tsf_voice *v) {
  int voiceIndex = tsf_voice_index(f, v);
  // if maxVoiceNum is set, assume that voice rendering and note queuing are on
  // separate threads so to minimize the chance that voice rendering would
  // advance the segment at the same time we just do it twice here and hope that
  // it sticks
  int repeats = (f->maxVoiceNum ? 2 : 1);
  while (repeats--) {
    v->ampenv.parameters.release = 0.0f;
    tsf_voice_envelope_nextsegment(&v->ampenv, TSF_SEGMENT_SUSTAIN,
                                   f->outSampleRate);
    v->modenv.parameters.release = 0.0f;
    tsf_voice_envelope_nextsegment(&v->modenv, TSF_SEGMENT_SUSTAIN,
                                   f->outSampleRate);
  }
  tsf_voice_update_hot_state(f, voiceIndex);
}

static void tsf_voice_calcpitchratio(struct tsf_voice *v, float pitchShift,
                                     float outSampleRate) {
  double note = v->playingKey + v->region->transpose + v->region->tune / 100.0;
  double adjustedPitch =
      v->region->pitch_keycenter +
      (note - v->region->pitch_keycenter) * (v->region->pitch_keytrack / 100.0);
  if (pitchShift)
    adjustedPitch += pitchShift;
  v->pitchInputTimecents = adjustedPitch * 100.0;
  v->pitchOutputFactor =
      v->region->sample_rate /
      (tsf_timecents2Secsd(v->region->pitch_keycenter * 100.0) * outSampleRate);
}

static TSF_BOOL tsf_runtime_has_avx2(void);

enum {
  TSF_RENDER_HELPER_CONTIGUOUS = 0,
  TSF_RENDER_HELPER_GATHER = 1,
  TSF_RENDER_HELPER_COMPLEX = 2
};

static void tsf_reset_perf_counters(tsf *f) {
  if (!f)
    return;
  f->perfHelperContiguousBlocks = 0u;
  f->perfHelperGatherBlocks = 0u;
  f->perfHelperComplexBlocks = 0u;
}

static int tsf_voice_render_helper_class(const tsf *f,
                                         const struct tsf_voice *v,
                                         int blockSamples) {
  const struct tsf_region *region;
  TSF_BOOL dynamicLowpass;
  TSF_BOOL dynamicPitchRatio;
  TSF_BOOL dynamicGain;
  TSF_BOOL isLooping;
  double pitchRatio;

  if (!f || !v || blockSamples < TSF_RENDER_MIN_SIMD_SAMPLES ||
      f->outputmode != TSF_STEREO_INTERLEAVED)
    return TSF_RENDER_HELPER_COMPLEX;

  region = v->region;
  if (!region)
    return TSF_RENDER_HELPER_COMPLEX;

  dynamicLowpass = (TSF_BOOL)(region->modLfoToFilterFc || region->modEnvToFilterFc);
  dynamicPitchRatio =
      (TSF_BOOL)(region->modLfoToPitch || region->modEnvToPitch ||
                 region->vibLfoToPitch);
  dynamicGain = (TSF_BOOL)(region->modLfoToVolume != 0);
  isLooping = (TSF_BOOL)(v->loopStart < v->loopEnd);
  if (dynamicLowpass || dynamicPitchRatio || dynamicGain || isLooping ||
      v->lowpass.active)
    return TSF_RENDER_HELPER_COMPLEX;

  pitchRatio = tsf_timecents2Secsd(v->pitchInputTimecents) * v->pitchOutputFactor;
  if (pitchRatio == 1.0 &&
      v->sourceSamplePosition == (double)(unsigned int)v->sourceSamplePosition)
    return TSF_RENDER_HELPER_CONTIGUOUS;
  return TSF_RENDER_HELPER_GATHER;
}

static void tsf_render_voice_plain_contiguous_avx2(
    float *input, float **outLPtr, int *blockSamplesPtr,
    double *tmpSourceSamplePositionPtr, double tmpSampleEndDbl, float gainLeft,
    float gainRight) {
  float *outL = *outLPtr;
  int blockSamples = *blockSamplesPtr;
  double tmpSourceSamplePosition = *tmpSourceSamplePositionPtr;

#if defined(_MSC_VER) || defined(__AVX2__)
  if (tsf_runtime_has_avx2() && blockSamples >= 8) {
    __m256 vGainLeft = _mm256_set1_ps(gainLeft);
    __m256 vGainRight = _mm256_set1_ps(gainRight);
    while (blockSamples >= 8 && tmpSourceSamplePosition + 8 < tmpSampleEndDbl) {
      unsigned int pos = (unsigned int)tmpSourceSamplePosition;
      __m256 vVal = _mm256_loadu_ps(input + pos);
      __m256 vLeft = _mm256_mul_ps(vVal, vGainLeft);
      __m256 vRight = _mm256_mul_ps(vVal, vGainRight);
      __m256 vLRLo = _mm256_unpacklo_ps(vLeft, vRight);
      __m256 vLRHi = _mm256_unpackhi_ps(vLeft, vRight);
      __m256 vOut0 = _mm256_permute2f128_ps(vLRLo, vLRHi, 0x20);
      __m256 vOut1 = _mm256_permute2f128_ps(vLRLo, vLRHi, 0x31);
      _mm256_storeu_ps(outL, _mm256_add_ps(_mm256_loadu_ps(outL), vOut0));
      _mm256_storeu_ps(outL + 8,
                       _mm256_add_ps(_mm256_loadu_ps(outL + 8), vOut1));
      outL += 16;
      tmpSourceSamplePosition += 8.0;
      blockSamples -= 8;
    }
  }
#endif

  {
    __m128 vGainLeft = _mm_set1_ps(gainLeft);
    __m128 vGainRight = _mm_set1_ps(gainRight);
    while (blockSamples >= 4 && tmpSourceSamplePosition + 4 < tmpSampleEndDbl) {
      unsigned int pos = (unsigned int)tmpSourceSamplePosition;
      __m128 vVal = _mm_loadu_ps(input + pos);
      __m128 vLeft = _mm_mul_ps(vVal, vGainLeft);
      __m128 vRight = _mm_mul_ps(vVal, vGainRight);
      __m128 vLR01 = _mm_unpacklo_ps(vLeft, vRight);
      __m128 vLR23 = _mm_unpackhi_ps(vLeft, vRight);
      _mm_storeu_ps(outL, _mm_add_ps(_mm_loadu_ps(outL), vLR01));
      _mm_storeu_ps(outL + 4, _mm_add_ps(_mm_loadu_ps(outL + 4), vLR23));
      outL += 8;
      tmpSourceSamplePosition += 4.0;
      blockSamples -= 4;
    }
  }

  while (blockSamples-- && tmpSourceSamplePosition < tmpSampleEndDbl) {
    unsigned int pos = (unsigned int)tmpSourceSamplePosition;
    float val = input[pos];
    *outL++ += val * gainLeft;
    *outL++ += val * gainRight;
    tmpSourceSamplePosition += 1.0;
  }

  if (blockSamples < 0)
    blockSamples = 0;

  *outLPtr = outL;
  *blockSamplesPtr = blockSamples;
  *tmpSourceSamplePositionPtr = tmpSourceSamplePosition;
}

static void tsf_render_voice_plain_gather_avx2(
    float *input, float **outLPtr, int *blockSamplesPtr,
    double *tmpSourceSamplePositionPtr, double tmpSampleEndDbl, double pitchRatio,
    float gainLeft, float gainRight) {
  float *outL = *outLPtr;
  int blockSamples = *blockSamplesPtr;
  double tmpSourceSamplePosition = *tmpSourceSamplePositionPtr;

#if defined(_MSC_VER) || defined(__AVX2__)
  if (tsf_runtime_has_avx2() && blockSamples >= 8) {
    __m256 vGainLeft = _mm256_set1_ps(gainLeft);
    __m256 vGainRight = _mm256_set1_ps(gainRight);
    __m256 vOne = _mm256_set1_ps(1.0f);
    __m256d vPitchStep = _mm256_set1_pd(pitchRatio);
    __m256d vLaneLo =
        _mm256_mul_pd(vPitchStep, _mm256_set_pd(3.0, 2.0, 1.0, 0.0));
    __m256d vLaneHi =
        _mm256_mul_pd(vPitchStep, _mm256_set_pd(7.0, 6.0, 5.0, 4.0));
    __m128i vOneI = _mm_set1_epi32(1);

    while (blockSamples >= 8 &&
           tmpSourceSamplePosition + pitchRatio * 8 < tmpSampleEndDbl) {
      __m256d vBasePos = _mm256_set1_pd(tmpSourceSamplePosition);
      __m256d vPosLoD = _mm256_add_pd(vBasePos, vLaneLo);
      __m256d vPosHiD = _mm256_add_pd(vBasePos, vLaneHi);
      __m128i vPosLoI128 = _mm256_cvttpd_epi32(vPosLoD);
      __m128i vPosHiI128 = _mm256_cvttpd_epi32(vPosHiD);
      __m128 vAlphaLo = _mm256_cvtpd_ps(
          _mm256_sub_pd(vPosLoD, _mm256_cvtepi32_pd(vPosLoI128)));
      __m128 vAlphaHi = _mm256_cvtpd_ps(
          _mm256_sub_pd(vPosHiD, _mm256_cvtepi32_pd(vPosHiI128)));
      __m256i vPosIndex = _mm256_insertf128_si256(
          _mm256_castsi128_si256(vPosLoI128), vPosHiI128, 1);
      __m256i vNextIndex = _mm256_insertf128_si256(
          _mm256_castsi128_si256(_mm_add_epi32(vPosLoI128, vOneI)),
          _mm_add_epi32(vPosHiI128, vOneI), 1);
      __m256 vAlpha =
          _mm256_insertf128_ps(_mm256_castps128_ps256(vAlphaLo), vAlphaHi, 1);
      __m256 vOneMinusAlpha = _mm256_sub_ps(vOne, vAlpha);
      __m256 vSamp0 = _mm256_i32gather_ps(input, vPosIndex, 4);
      __m256 vSamp1 = _mm256_i32gather_ps(input, vNextIndex, 4);
      __m256 vVal = _mm256_add_ps(_mm256_mul_ps(vSamp0, vOneMinusAlpha),
                                  _mm256_mul_ps(vSamp1, vAlpha));
      __m256 vLeft = _mm256_mul_ps(vVal, vGainLeft);
      __m256 vRight = _mm256_mul_ps(vVal, vGainRight);
      __m256 vLRLo = _mm256_unpacklo_ps(vLeft, vRight);
      __m256 vLRHi = _mm256_unpackhi_ps(vLeft, vRight);
      __m256 vOut0 = _mm256_permute2f128_ps(vLRLo, vLRHi, 0x20);
      __m256 vOut1 = _mm256_permute2f128_ps(vLRLo, vLRHi, 0x31);
      _mm256_storeu_ps(outL, _mm256_add_ps(_mm256_loadu_ps(outL), vOut0));
      _mm256_storeu_ps(outL + 8,
                       _mm256_add_ps(_mm256_loadu_ps(outL + 8), vOut1));
      outL += 16;
      tmpSourceSamplePosition += pitchRatio * 8;
      blockSamples -= 8;
    }
  }
#endif

  {
    __m128 vGainLeft = _mm_set1_ps(gainLeft);
    __m128 vGainRight = _mm_set1_ps(gainRight);
    __m128 vOne = _mm_set1_ps(1.0f);
    while (blockSamples >= 4 &&
           tmpSourceSamplePosition + pitchRatio * 4 < tmpSampleEndDbl) {
      unsigned int pos0 = (unsigned int)tmpSourceSamplePosition;
      unsigned int pos1 = (unsigned int)(tmpSourceSamplePosition + pitchRatio);
      unsigned int pos2 =
          (unsigned int)(tmpSourceSamplePosition + pitchRatio * 2);
      unsigned int pos3 =
          (unsigned int)(tmpSourceSamplePosition + pitchRatio * 3);
      float alpha0 = (float)(tmpSourceSamplePosition - pos0);
      float alpha1 = (float)(tmpSourceSamplePosition + pitchRatio - pos1);
      float alpha2 = (float)(tmpSourceSamplePosition + pitchRatio * 2 - pos2);
      float alpha3 = (float)(tmpSourceSamplePosition + pitchRatio * 3 - pos3);
      __m128 vAlpha = _mm_set_ps(alpha3, alpha2, alpha1, alpha0);
      __m128 vOneMinusAlpha = _mm_sub_ps(vOne, vAlpha);
      __m128 vSamp0 =
          _mm_set_ps(input[pos3], input[pos2], input[pos1], input[pos0]);
      __m128 vSamp1 = _mm_set_ps(input[pos3 + 1], input[pos2 + 1],
                                 input[pos1 + 1], input[pos0 + 1]);
      __m128 vVal = _mm_add_ps(_mm_mul_ps(vSamp0, vOneMinusAlpha),
                               _mm_mul_ps(vSamp1, vAlpha));
      __m128 vLeft = _mm_mul_ps(vVal, vGainLeft);
      __m128 vRight = _mm_mul_ps(vVal, vGainRight);
      __m128 vLR01 = _mm_unpacklo_ps(vLeft, vRight);
      __m128 vLR23 = _mm_unpackhi_ps(vLeft, vRight);
      _mm_storeu_ps(outL, _mm_add_ps(_mm_loadu_ps(outL), vLR01));
      _mm_storeu_ps(outL + 4, _mm_add_ps(_mm_loadu_ps(outL + 4), vLR23));
      outL += 8;
      tmpSourceSamplePosition += pitchRatio * 4;
      blockSamples -= 4;
    }
  }

  while (blockSamples-- && tmpSourceSamplePosition < tmpSampleEndDbl) {
    unsigned int pos = (unsigned int)tmpSourceSamplePosition;
    float alpha = (float)(tmpSourceSamplePosition - pos);
    float val = (input[pos] * (1.0f - alpha) + input[pos + 1] * alpha);
    *outL++ += val * gainLeft;
    *outL++ += val * gainRight;
    tmpSourceSamplePosition += pitchRatio;
  }

  if (blockSamples < 0)
    blockSamples = 0;

  *outLPtr = outL;
  *blockSamplesPtr = blockSamples;
  *tmpSourceSamplePositionPtr = tmpSourceSamplePosition;
}

static void tsf_render_voice_scalar_complex(
    tsf *f, float *input, float **outLPtr, float **outRPtr, int *blockSamplesPtr,
    double *tmpSourceSamplePositionPtr, double tmpSampleEndDbl,
    double tmpLoopEndDbl, unsigned int tmpLoopStart, unsigned int tmpLoopEnd,
    TSF_BOOL isLooping, struct tsf_voice_lowpass *tmpLowpassPtr, double pitchRatio,
    float gainMono, float gainLeft, float gainRight) {
  float *outL = *outLPtr;
  float *outR = *outRPtr;
  int blockSamples = *blockSamplesPtr;
  double tmpSourceSamplePosition = *tmpSourceSamplePositionPtr;
  struct tsf_voice_lowpass *tmpLowpass = tmpLowpassPtr;

  switch (f->outputmode) {
  case TSF_STEREO_INTERLEAVED:
    while (blockSamples-- && tmpSourceSamplePosition < tmpSampleEndDbl) {
      unsigned int pos = (unsigned int)tmpSourceSamplePosition;
      unsigned int nextPos =
          (pos >= tmpLoopEnd && isLooping ? tmpLoopStart : pos + 1);
      float alpha = (float)(tmpSourceSamplePosition - pos);
      float val = (input[pos] * (1.0f - alpha) + input[nextPos] * alpha);
      if (tmpLowpass->active)
        val = tsf_voice_lowpass_process(tmpLowpass, val);
      *outL++ += val * gainLeft;
      *outL++ += val * gainRight;
      tmpSourceSamplePosition += pitchRatio;
      if (tmpSourceSamplePosition >= tmpLoopEndDbl && isLooping)
        tmpSourceSamplePosition -= (tmpLoopEnd - tmpLoopStart + 1.0);
    }
    break;

  case TSF_STEREO_UNWEAVED:
    while (blockSamples-- && tmpSourceSamplePosition < tmpSampleEndDbl) {
      unsigned int pos = (unsigned int)tmpSourceSamplePosition;
      unsigned int nextPos =
          (pos >= tmpLoopEnd && isLooping ? tmpLoopStart : pos + 1);
      float alpha = (float)(tmpSourceSamplePosition - pos);
      float val = (input[pos] * (1.0f - alpha) + input[nextPos] * alpha);
      if (tmpLowpass->active)
        val = tsf_voice_lowpass_process(tmpLowpass, val);
      *outL++ += val * gainLeft;
      *outR++ += val * gainRight;
      tmpSourceSamplePosition += pitchRatio;
      if (tmpSourceSamplePosition >= tmpLoopEndDbl && isLooping)
        tmpSourceSamplePosition -= (tmpLoopEnd - tmpLoopStart + 1.0);
    }
    break;

  default:
    while (blockSamples-- && tmpSourceSamplePosition < tmpSampleEndDbl) {
      unsigned int pos = (unsigned int)tmpSourceSamplePosition;
      unsigned int nextPos =
          (pos >= tmpLoopEnd && isLooping ? tmpLoopStart : pos + 1);
      float alpha = (float)(tmpSourceSamplePosition - pos);
      float val = (input[pos] * (1.0f - alpha) + input[nextPos] * alpha);
      if (tmpLowpass->active)
        val = tsf_voice_lowpass_process(tmpLowpass, val);
      *outL++ += val * gainMono;
      tmpSourceSamplePosition += pitchRatio;
      if (tmpSourceSamplePosition >= tmpLoopEndDbl && isLooping)
        tmpSourceSamplePosition -= (tmpLoopEnd - tmpLoopStart + 1.0);
    }
    break;
  }

  if (blockSamples < 0)
    blockSamples = 0;

  *outLPtr = outL;
  *outRPtr = outR;
  *blockSamplesPtr = blockSamples;
  *tmpSourceSamplePositionPtr = tmpSourceSamplePosition;
}

static void tsf_voice_render(tsf *f, struct tsf_voice *v, float *outputBuffer,
                             int numSamples) {
  struct tsf_region *region = v->region;
  float *input = f->fontSamples;
  float *outL = outputBuffer;
  float *outR =
      (f->outputmode == TSF_STEREO_UNWEAVED ? outL + numSamples : TSF_NULL);

  // Cache some values, to give them at least some chance of ending up in
  // registers.
  TSF_BOOL updateModEnv = (region->modEnvToPitch || region->modEnvToFilterFc);
  TSF_BOOL updateModLFO =
      (v->modlfo.delta && (region->modLfoToPitch || region->modLfoToFilterFc ||
                           region->modLfoToVolume));
  TSF_BOOL updateVibLFO = (v->viblfo.delta && (region->vibLfoToPitch));
  TSF_BOOL isLooping = (v->loopStart < v->loopEnd);
  unsigned int tmpLoopStart = v->loopStart, tmpLoopEnd = v->loopEnd;
  double tmpSampleEndDbl = (double)region->end,
         tmpLoopEndDbl = (double)tmpLoopEnd + 1.0;
  double tmpSourceSamplePosition = v->sourceSamplePosition;
  struct tsf_voice_lowpass tmpLowpass = v->lowpass;

  TSF_BOOL dynamicLowpass =
      (region->modLfoToFilterFc || region->modEnvToFilterFc);
  float tmpSampleRate = f->outSampleRate, tmpInitialFilterFc,
        tmpModLfoToFilterFc, tmpModEnvToFilterFc;

  TSF_BOOL dynamicPitchRatio =
      (region->modLfoToPitch || region->modEnvToPitch || region->vibLfoToPitch);
  double pitchRatio;
  float tmpModLfoToPitch, tmpVibLfoToPitch, tmpModEnvToPitch;

  TSF_BOOL dynamicGain = (region->modLfoToVolume != 0);
  float noteGain = 0, tmpModLfoToVolume;

  if (dynamicLowpass)
    tmpInitialFilterFc = (float)region->initialFilterFc,
    tmpModLfoToFilterFc = (float)region->modLfoToFilterFc,
    tmpModEnvToFilterFc = (float)region->modEnvToFilterFc;
  else
    tmpInitialFilterFc = 0, tmpModLfoToFilterFc = 0, tmpModEnvToFilterFc = 0;

  if (dynamicPitchRatio)
    pitchRatio = 0, tmpModLfoToPitch = (float)region->modLfoToPitch,
    tmpVibLfoToPitch = (float)region->vibLfoToPitch,
    tmpModEnvToPitch = (float)region->modEnvToPitch;
  else
    pitchRatio =
        tsf_timecents2Secsd(v->pitchInputTimecents) * v->pitchOutputFactor,
    tmpModLfoToPitch = 0, tmpVibLfoToPitch = 0, tmpModEnvToPitch = 0;

  if (dynamicGain)
    tmpModLfoToVolume = (float)region->modLfoToVolume * 0.1f;
  else
    noteGain = tsf_decibelsToGain(v->noteGainDB), tmpModLfoToVolume = 0;

  while (numSamples) {
    float gainMono, gainLeft, gainRight;
    int blockSamples = (numSamples > TSF_RENDER_EFFECTSAMPLEBLOCK
                            ? TSF_RENDER_EFFECTSAMPLEBLOCK
                            : numSamples);
    numSamples -= blockSamples;

    if (dynamicLowpass) {
      float fres = tmpInitialFilterFc + v->modlfo.level * tmpModLfoToFilterFc +
                   v->modenv.level * tmpModEnvToFilterFc;
      float lowpassFc =
          (fres <= 13500 ? tsf_cents2Hertz(fres) / tmpSampleRate : 1.0f);
      tmpLowpass.active = (lowpassFc < 0.499f);
      if (tmpLowpass.active)
        tsf_voice_lowpass_setup(&tmpLowpass, lowpassFc);
    }

    if (dynamicPitchRatio)
      pitchRatio = tsf_timecents2Secsd(v->pitchInputTimecents +
                                       (v->modlfo.level * tmpModLfoToPitch +
                                        v->viblfo.level * tmpVibLfoToPitch +
                                        v->modenv.level * tmpModEnvToPitch)) *
                   v->pitchOutputFactor;

    if (dynamicGain)
      noteGain = tsf_decibelsToGain(v->noteGainDB +
                                    (v->modlfo.level * tmpModLfoToVolume));

    gainMono = noteGain * v->ampenv.level;

    // Update EG.
    tsf_voice_envelope_process(&v->ampenv, blockSamples, tmpSampleRate);
    if (updateModEnv)
      tsf_voice_envelope_process(&v->modenv, blockSamples, tmpSampleRate);

    // Update LFOs.
    if (updateModLFO)
      tsf_voice_lfo_process(&v->modlfo, blockSamples);
    if (updateVibLFO)
      tsf_voice_lfo_process(&v->viblfo, blockSamples);

    gainLeft = gainMono * v->panFactorLeft;
    gainRight = gainMono * v->panFactorRight;

    if (f->outputmode == TSF_STEREO_INTERLEAVED) {
      int helperClass =
          tsf_voice_render_helper_class(f, v, blockSamples);
      if (helperClass == TSF_RENDER_HELPER_CONTIGUOUS) {
#if SVMS_PERF_DEBUG
        ++f->perfHelperContiguousBlocks;
#endif
        tsf_render_voice_plain_contiguous_avx2(
            input, &outL, &blockSamples, &tmpSourceSamplePosition, tmpSampleEndDbl,
            gainLeft, gainRight);
      } else if (helperClass == TSF_RENDER_HELPER_GATHER) {
#if SVMS_PERF_DEBUG
        ++f->perfHelperGatherBlocks;
#endif
        tsf_render_voice_plain_gather_avx2(
            input, &outL, &blockSamples, &tmpSourceSamplePosition, tmpSampleEndDbl,
            pitchRatio, gainLeft, gainRight);
      } else {
#if SVMS_PERF_DEBUG
        ++f->perfHelperComplexBlocks;
#endif
        tsf_render_voice_scalar_complex(
            f, input, &outL, &outR, &blockSamples, &tmpSourceSamplePosition,
            tmpSampleEndDbl, tmpLoopEndDbl, tmpLoopStart, tmpLoopEnd, isLooping,
            &tmpLowpass, pitchRatio, gainMono, gainLeft, gainRight);
      }
    } else {
#if SVMS_PERF_DEBUG
      ++f->perfHelperComplexBlocks;
#endif
      tsf_render_voice_scalar_complex(
          f, input, &outL, &outR, &blockSamples, &tmpSourceSamplePosition,
          tmpSampleEndDbl, tmpLoopEndDbl, tmpLoopStart, tmpLoopEnd, isLooping,
          &tmpLowpass, pitchRatio, gainMono, gainLeft, gainRight);
    }

    if (tmpSourceSamplePosition >= tmpSampleEndDbl ||
        v->ampenv.segment == TSF_SEGMENT_DONE) {
      tsf_voice_kill(f, v);
      return;
    }
  }

  v->sourceSamplePosition = tmpSourceSamplePosition;
  if (tmpLowpass.active || dynamicLowpass)
    v->lowpass = tmpLowpass;
  tsf_voice_update_hot_state(f, tsf_voice_index(f, v));
}

TSFDEF tsf *tsf_load(struct tsf_stream *stream) {
  tsf *res = TSF_NULL;
  struct tsf_riffchunk chunkHead;
  struct tsf_riffchunk chunkList;
  struct tsf_hydra hydra;
  void *rawBuffer = TSF_NULL;
  float *floatBuffer = TSF_NULL;
  tsf_u32 smplCount = 0;

  if (!tsf_riffchunk_read(TSF_NULL, &chunkHead, stream) ||
      !TSF_FourCCEquals(chunkHead.id, "sfbk")) {
    // if (e) *e = TSF_INVALID_NOSF2HEADER;
    return res;
  }

  // Read hydra and locate sample data.
  TSF_MEMSET(&hydra, 0, sizeof(hydra));
  while (tsf_riffchunk_read(&chunkHead, &chunkList, stream)) {
    struct tsf_riffchunk chunk;
    if (TSF_FourCCEquals(chunkList.id, "pdta")) {
      while (tsf_riffchunk_read(&chunkList, &chunk, stream)) {
#define HandleChunk(chunkName)                                                 \
  (TSF_FourCCEquals(chunk.id, #chunkName) &&                                   \
   !(chunk.size % chunkName##SizeInFile)) {                                    \
    int num = chunk.size / chunkName##SizeInFile, i;                           \
    hydra.chunkName##Num = num;                                                \
    hydra.chunkName##s = (struct tsf_hydra_##chunkName *)TSF_MALLOC(           \
        num * sizeof(struct tsf_hydra_##chunkName));                           \
    if (!hydra.chunkName##s)                                                   \
      goto out_of_memory;                                                      \
    for (i = 0; i < num; ++i)                                                  \
      tsf_hydra_read_##chunkName(&hydra.chunkName##s[i], stream);              \
  }
        enum {
          phdrSizeInFile = 38,
          pbagSizeInFile = 4,
          pmodSizeInFile = 10,
          pgenSizeInFile = 4,
          instSizeInFile = 22,
          ibagSizeInFile = 4,
          imodSizeInFile = 10,
          igenSizeInFile = 4,
          shdrSizeInFile = 46
        };
        if HandleChunk (phdr)
          else if HandleChunk (pbag) else if HandleChunk (pmod) else if HandleChunk (pgen) else if HandleChunk (inst) else if HandleChunk (
              ibag) else if HandleChunk (imod) else if HandleChunk (igen) else if HandleChunk (shdr) else stream
              ->skip(stream->data, chunk.size);
#undef HandleChunk
      }
    } else if (TSF_FourCCEquals(chunkList.id, "sdta")) {
      while (tsf_riffchunk_read(&chunkList, &chunk, stream)) {
        if ((TSF_FourCCEquals(chunk.id, "smpl")
#ifdef STB_VORBIS_INCLUDE_STB_VORBIS_H
             || TSF_FourCCEquals(chunk.id, "smpo")
#endif
                 ) &&
            !rawBuffer && !floatBuffer && chunk.size >= sizeof(short)) {
          if (!tsf_load_samples(&rawBuffer, &floatBuffer, &smplCount, &chunk,
                                stream))
            goto out_of_memory;
        } else
          stream->skip(stream->data, chunk.size);
      }
    } else
      stream->skip(stream->data, chunkList.size);
  }
  if (!hydra.phdrs || !hydra.pbags || !hydra.pmods || !hydra.pgens ||
      !hydra.insts || !hydra.ibags || !hydra.imods || !hydra.igens ||
      !hydra.shdrs) {
    // if (e) *e = TSF_INVALID_INCOMPLETE;
  } else if (!rawBuffer && !floatBuffer) {
    // if (e) *e = TSF_INVALID_NOSAMPLEDATA;
  } else {
#ifdef STB_VORBIS_INCLUDE_STB_VORBIS_H
    if (!floatBuffer &&
        !tsf_decode_sf3_samples(rawBuffer, &floatBuffer, &smplCount, &hydra))
      goto out_of_memory;
#endif
    res = (tsf *)TSF_MALLOC(sizeof(tsf));
    if (res)
      TSF_MEMSET(res, 0, sizeof(tsf));
    if (!res || !tsf_load_presets(res, &hydra, smplCount))
      goto out_of_memory;
    tsf_clear_channel_voice_cache(res);
    res->outSampleRate = 44100.0f;
    res->fontSamples = floatBuffer;
    floatBuffer = TSF_NULL; // don't free below
  }
  if (0) {
  out_of_memory:
    TSF_FREE(res);
    res = TSF_NULL;
    // if (e) *e = TSF_OUT_OF_MEMORY;
  }
  TSF_FREE(hydra.phdrs);
  TSF_FREE(hydra.pbags);
  TSF_FREE(hydra.pmods);
  TSF_FREE(hydra.pgens);
  TSF_FREE(hydra.insts);
  TSF_FREE(hydra.ibags);
  TSF_FREE(hydra.imods);
  TSF_FREE(hydra.igens);
  TSF_FREE(hydra.shdrs);
  TSF_FREE(rawBuffer);
  TSF_FREE(floatBuffer);
  return res;
}

TSFDEF tsf *tsf_copy(tsf *f) {
  tsf *res;
  if (!f)
    return TSF_NULL;
  if (!f->refCount) {
    f->refCount = (int *)TSF_MALLOC(sizeof(int));
    if (!f->refCount)
      return TSF_NULL;
    *f->refCount = 1;
  }
  res = (tsf *)TSF_MALLOC(sizeof(tsf));
  if (!res)
    return TSF_NULL;
  TSF_MEMCPY(res, f, sizeof(tsf));
  res->voices = TSF_NULL;
  res->voiceSourceSamplePosition = TSF_NULL;
  res->voiceBasePitchRatio = TSF_NULL;
  res->voicePitchRatio = TSF_NULL;
  res->voiceBaseVolumeL = TSF_NULL;
  res->voiceBaseVolumeR = TSF_NULL;
  res->voiceVolumeL = TSF_NULL;
  res->voiceVolumeR = TSF_NULL;
  res->voiceAmpEnvLevel = TSF_NULL;
  res->voiceAmpEnvSlope = TSF_NULL;
  res->voiceModEnvLevel = TSF_NULL;
  res->voiceModEnvSlope = TSF_NULL;
  res->voiceModLfoLevel = TSF_NULL;
  res->voiceModLfoDelta = TSF_NULL;
  res->voiceVibLfoLevel = TSF_NULL;
  res->voiceVibLfoDelta = TSF_NULL;
  res->voiceFilterZ1Left = TSF_NULL;
  res->voiceFilterZ2Left = TSF_NULL;
  res->voiceFilterZ1Right = TSF_NULL;
  res->voiceFilterZ2Right = TSF_NULL;
  res->voiceFilterA0 = TSF_NULL;
  res->voiceFilterA1 = TSF_NULL;
  res->voiceFilterB1 = TSF_NULL;
  res->voiceFilterB2 = TSF_NULL;
  res->voiceAmpEnvSamplesUntilNextSegment = TSF_NULL;
  res->voiceModEnvSamplesUntilNextSegment = TSF_NULL;
  res->voiceModLfoSamplesUntil = TSF_NULL;
  res->voiceVibLfoSamplesUntil = TSF_NULL;
  res->voiceFilterActive = TSF_NULL;
  res->voiceState = TSF_NULL;
  res->activeVoiceIndices = TSF_NULL;
  res->freeVoiceIndices = TSF_NULL;
  res->exclusiveGroupKeys = TSF_NULL;
  res->exclusiveGroupHeads = TSF_NULL;
  res->exclusiveGroupCapacity = 0;
  res->exclusiveGroupCount = 0;
  res->exclusiveGroupTombstones = 0;
  res->voiceNum = 0;
  res->activeVoiceCount = 0;
  res->freeVoiceCount = 0;
  res->channels = TSF_NULL;
  tsf_clear_channel_voice_cache(res);
  (*res->refCount)++;
  return res;
}

TSFDEF void tsf_close(tsf *f) {
  if (!f)
    return;
  if (!f->refCount || !--(*f->refCount)) {
    struct tsf_preset *preset = f->presets, *presetEnd = preset + f->presetNum;
    for (; preset != presetEnd; preset++) {
      TSF_FREE(preset->regions);
      tsf_preset_free_key_dispatch(preset);
    }
    TSF_FREE(f->presets);
    TSF_FREE(f->fontSamples);
    TSF_FREE(f->refCount);
  }
  TSF_FREE(f->channels);
  TSF_FREE(f->activeVoiceIndices);
  TSF_FREE(f->freeVoiceIndices);
  TSF_FREE(f->exclusiveGroupKeys);
  TSF_FREE(f->exclusiveGroupHeads);
  tsf_aligned_free(f->voiceSourceSamplePosition);
  tsf_aligned_free(f->voiceBasePitchRatio);
  tsf_aligned_free(f->voicePitchRatio);
  tsf_aligned_free(f->voiceBaseVolumeL);
  tsf_aligned_free(f->voiceBaseVolumeR);
  tsf_aligned_free(f->voiceVolumeL);
  tsf_aligned_free(f->voiceVolumeR);
  tsf_aligned_free(f->voiceAmpEnvLevel);
  tsf_aligned_free(f->voiceAmpEnvSlope);
  tsf_aligned_free(f->voiceModEnvLevel);
  tsf_aligned_free(f->voiceModEnvSlope);
  tsf_aligned_free(f->voiceModLfoLevel);
  tsf_aligned_free(f->voiceModLfoDelta);
  tsf_aligned_free(f->voiceVibLfoLevel);
  tsf_aligned_free(f->voiceVibLfoDelta);
  tsf_aligned_free(f->voiceFilterZ1Left);
  tsf_aligned_free(f->voiceFilterZ2Left);
  tsf_aligned_free(f->voiceFilterZ1Right);
  tsf_aligned_free(f->voiceFilterZ2Right);
  tsf_aligned_free(f->voiceFilterA0);
  tsf_aligned_free(f->voiceFilterA1);
  tsf_aligned_free(f->voiceFilterB1);
  tsf_aligned_free(f->voiceFilterB2);
  tsf_aligned_free(f->voiceAmpEnvSamplesUntilNextSegment);
  tsf_aligned_free(f->voiceModEnvSamplesUntilNextSegment);
  tsf_aligned_free(f->voiceModLfoSamplesUntil);
  tsf_aligned_free(f->voiceVibLfoSamplesUntil);
  tsf_aligned_free(f->voiceFilterActive);
  tsf_aligned_free(f->voiceState);
  TSF_FREE(f->voices);
  TSF_FREE(f);
}

TSFDEF void tsf_reset(tsf *f) {
  int i;
  for (i = 0; i < f->activeVoiceCount; i++) {
    struct tsf_voice *v = &f->voices[f->activeVoiceIndices[i]];
    if (v->playingPreset != -1 &&
        (v->ampenv.segment < TSF_SEGMENT_RELEASE ||
         v->ampenv.parameters.release))
      tsf_voice_endquick(f, v);
  }
  tsf_clear_channel_voice_cache(f);
  if (f->channels) {
    TSF_FREE(f->channels);
    f->channels = TSF_NULL;
  }
}

TSFDEF int tsf_get_presetindex(const tsf *f, int bank, int preset_number) {
  const struct tsf_preset *presets;
  int i, iMax;
  for (presets = f->presets, i = 0, iMax = f->presetNum; i < iMax; i++)
    if (presets[i].preset == preset_number && presets[i].bank == bank)
      return i;
  return -1;
}

TSFDEF int tsf_get_presetcount(const tsf *f) { return f->presetNum; }

TSFDEF const char *tsf_get_presetname(const tsf *f, int preset) {
  return (preset < 0 || preset >= f->presetNum ? TSF_NULL
                                               : f->presets[preset].presetName);
}

TSFDEF const char *tsf_bank_get_presetname(const tsf *f, int bank,
                                           int preset_number) {
  return tsf_get_presetname(f, tsf_get_presetindex(f, bank, preset_number));
}

TSFDEF void tsf_set_output(tsf *f, enum TSFOutputMode outputmode,
                           int samplerate, float global_gain_db) {
  f->outputmode = outputmode;
  f->outSampleRate = (float)(samplerate >= 1 ? samplerate : 44100.0f);
  f->globalGainDB = global_gain_db;
}

TSFDEF void tsf_set_volume(tsf *f, float global_volume) {
  f->globalGainDB =
      (global_volume == 1.0f ? 0 : -tsf_gainToDecibels(1.0f / global_volume));
}

TSFDEF int tsf_set_max_voices(tsf *f, int max_voices) {
  int newVoiceNum = (f->voiceNum > max_voices ? f->voiceNum : max_voices);
  if (!tsf_reserve_voice_storage(f, newVoiceNum))
    return 0;
  f->maxVoiceNum = max_voices;
  return 1;
}

TSFDEF int tsf_note_on(tsf *f, int preset_index, int key, float vel) {
  return tsf_note_on_ex(f, preset_index, key, vel, (int)(vel * 127.0f));
}

TSFDEF int tsf_note_on_ex(tsf *f, int preset_index, int key, float gain_vel,
                          int midi_velocity) {
  short midiVelocity = (short)midi_velocity;
  unsigned int voicePlayIndex;
  struct tsf_preset *preset;
  int candidateOffset, candidateCount, candidateIndex;

  if (preset_index < 0 || preset_index >= f->presetNum)
    return 1;
  if (gain_vel <= 0.0f) {
    tsf_note_off(f, preset_index, key);
    return 1;
  }

  if (midiVelocity < 0)
    midiVelocity = 0;
  else if (midiVelocity > 127)
    midiVelocity = 127;

  if (gain_vel > 1.0f)
    gain_vel = 1.0f;

  preset = &f->presets[preset_index];
  candidateOffset = preset->keyRegionOffsets[key];
  candidateCount = preset->keyRegionCounts[key];

  // Play all matching regions.
  voicePlayIndex = f->voicePlayIndex++;
  for (candidateIndex = 0; candidateIndex < candidateCount; candidateIndex++) {
    struct tsf_region *region =
        &preset->regions[preset->keyRegionIndices[candidateOffset +
                                                  candidateIndex]];
    struct tsf_voice *voice;
    TSF_BOOL doLoop;
    float lowpassFilterQDB, lowpassFc;
    if (midiVelocity < region->lovel || midiVelocity > region->hivel)
      continue;

    voice = TSF_NULL;
    if (region->group) {
      unsigned long long groupKey =
          tsf_make_exclusive_group_key(preset_index, region->group);
      if (groupKey != TSF_EXCLUSIVE_GROUP_KEY_EMPTY &&
          tsf_ensure_exclusive_group_cache_capacity(
              f, (unsigned int)f->activeVoiceCount + 1u)) {
        int slot = tsf_exclusive_group_lookup_slot(f, groupKey);
        if (slot >= 0) {
          int groupVoiceIndex = f->exclusiveGroupHeads[slot];
          while (groupVoiceIndex != -1) {
            struct tsf_voice *v = &f->voices[groupVoiceIndex];
            int nextGroupVoice = v->nextExclusiveGroupVoice;
            if (v->playingPreset == preset_index && v->region &&
                v->region->group == region->group)
              tsf_voice_endquick(f, v);
            groupVoiceIndex = nextGroupVoice;
          }
        }
      } else {
        int i;
        for (i = 0; i < f->activeVoiceCount; i++) {
          struct tsf_voice *v = &f->voices[f->activeVoiceIndices[i]];
          if (v->playingPreset == preset_index && v->region &&
              v->region->group == region->group)
            tsf_voice_endquick(f, v);
        }
      }
    }

    {
      int voiceIndex = tsf_acquire_free_voice_index(f);
      if (voiceIndex >= 0) {
        tsf_activate_voice_index(f, voiceIndex);
        voice = &f->voices[voiceIndex];
      }
    }

    if (!voice) {
      if (f->maxVoiceNum) {
        // Quiet voices are sacrificed first. Among equally quiet candidates,
        // releasing voices are preferred before older active ones.
        voice = tsf_select_voice_to_steal(f);
        if (!voice)
          continue;
        tsf_voice_kill(f, voice);
      } else {
        // Allocate more voices so we don't need to kill one off.
        int voiceIndex;
        if (!tsf_reserve_voice_storage(f, f->voiceNum + 4))
          return 0;
        voiceIndex = tsf_acquire_free_voice_index(f);
        if (voiceIndex < 0)
          return 0;
        tsf_activate_voice_index(f, voiceIndex);
        voice = &f->voices[voiceIndex];
      }
    }

    voice->pendingFree = 0;
    voice->region = region;
    voice->playingPreset = preset_index;
    voice->playingKey = key;
    voice->playIndex = voicePlayIndex;
    voice->heldSustain = 0;
    voice->midiVelocity = midiVelocity;
    voice->noteGainDB =
        f->globalGainDB - region->attenuation -
        tsf_gainToDecibels(1.0f / gain_vel);

    if (f->channels) {
      f->channels->setupVoice(f, voice);
    } else {
      voice->playingChannel = -1;
      tsf_voice_calcpitchratio(voice, 0, f->outSampleRate);
      // The SFZ spec is silent about the pan curve, but a 3dB pan law seems
      // common. This sqrt() curve matches what Dimension LE does; Alchemy Free
      // seems closer to sin(adjustedPan * pi/2).
      voice->panFactorLeft = TSF_SQRTF(0.5f - region->pan);
      voice->panFactorRight = TSF_SQRTF(0.5f + region->pan);
    }
    tsf_link_voice_to_channel(f, tsf_voice_index(f, voice));

    // Offset/end.
    voice->sourceSamplePosition = region->offset;

    // Loop.
    doLoop = (region->loop_mode != TSF_LOOPMODE_NONE &&
              region->loop_start < region->loop_end);
    voice->loopStart = (doLoop ? region->loop_start : 0);
    voice->loopEnd = (doLoop ? region->loop_end : 0);

    // Setup envelopes.
    tsf_voice_envelope_setup(&voice->ampenv, &region->ampenv, key, midiVelocity,
                             TSF_TRUE, f->outSampleRate);
    tsf_voice_envelope_setup(&voice->modenv, &region->modenv, key, midiVelocity,
                             TSF_FALSE, f->outSampleRate);

    // Setup lowpass filter.
    lowpassFc = (region->initialFilterFc <= 13500
                     ? tsf_cents2Hertz((float)region->initialFilterFc) /
                           f->outSampleRate
                     : 1.0f);
    lowpassFilterQDB = region->initialFilterQ / 10.0f;
    voice->lowpass.QInv = 1.0 / TSF_POW(10.0, (lowpassFilterQDB / 20.0));
    voice->lowpass.z1 = voice->lowpass.z2 = 0;
    voice->lowpass.active = (lowpassFc < 0.499f);
    if (voice->lowpass.active)
      tsf_voice_lowpass_setup(&voice->lowpass, lowpassFc);

    // Setup LFO filters.
    tsf_voice_lfo_setup(&voice->modlfo, region->delayModLFO, region->freqModLFO,
                        f->outSampleRate);
    tsf_voice_lfo_setup(&voice->viblfo, region->delayVibLFO, region->freqVibLFO,
                        f->outSampleRate);
    tsf_voice_update_hot_state(f, tsf_voice_index(f, voice));
    tsf_link_voice_to_exclusive_group(f, tsf_voice_index(f, voice));
  }
  return 1;
}

TSFDEF int tsf_bank_note_on(tsf *f, int bank, int preset_number, int key,
                            float vel) {
  int preset_index = tsf_get_presetindex(f, bank, preset_number);
  if (preset_index == -1)
    return 0;
  return tsf_note_on(f, preset_index, key, vel);
}

TSFDEF void tsf_note_off(tsf *f, int preset_index, int key) {
  unsigned int matchPlayIndex = 0;
  TSF_BOOL found = TSF_FALSE;
  int i;
  for (i = 0; i < f->activeVoiceCount; i++) {
    struct tsf_voice *v = &f->voices[f->activeVoiceIndices[i]];
    if (v->playingPreset != preset_index || v->playingKey != key ||
        v->ampenv.segment >= TSF_SEGMENT_RELEASE)
      continue;
    if (!found || v->playIndex < matchPlayIndex) {
      matchPlayIndex = v->playIndex;
      found = TSF_TRUE;
    }
  }
  if (!found)
    return;
  for (i = 0; i < f->activeVoiceCount; i++) {
    struct tsf_voice *v = &f->voices[f->activeVoiceIndices[i]];
    if (v->playingPreset == preset_index && v->playingKey == key &&
        v->playIndex == matchPlayIndex &&
        v->ampenv.segment < TSF_SEGMENT_RELEASE)
      tsf_voice_end(f, v);
  }
}

TSFDEF int tsf_bank_note_off(tsf *f, int bank, int preset_number, int key) {
  int preset_index = tsf_get_presetindex(f, bank, preset_number);
  if (preset_index == -1)
    return 0;
  tsf_note_off(f, preset_index, key);
  return 1;
}

TSFDEF void tsf_note_off_all(tsf *f) {
  int i;
  for (i = 0; i < f->activeVoiceCount; i++) {
    struct tsf_voice *v = &f->voices[f->activeVoiceIndices[i]];
    if (v->playingPreset != -1 && v->ampenv.segment < TSF_SEGMENT_RELEASE)
      tsf_voice_end(f, v);
  }
}

TSFDEF int tsf_active_voice_count(tsf *f) {
  return f->activeVoiceCount;
}

TSFDEF const int *tsf_get_active_voice_indices(const tsf *f) {
  return f->activeVoiceIndices;
}

TSFDEF void tsf_get_active_voice_channel_counts(const tsf *f, int *counts,
                                                int count) {
  int i;
  if (!counts || count <= 0)
    return;
  for (i = 0; i < count; ++i)
    counts[i] = 0;
  tsf_ensure_channel_voice_cache((tsf *)f);
  for (i = 0; i < count && i < 16; ++i)
    counts[i] = f->channelVoiceCounts[i];
}

TSFDEF void tsf_cleanup_inactive_voices(tsf *f) {
  int writeIndex = 0;
  int i;
  tsf_clear_channel_voice_cache(f);
  for (i = 0; i < f->activeVoiceCount; i++) {
    int voiceIndex = f->activeVoiceIndices[i];
    struct tsf_voice *v = &f->voices[voiceIndex];
    if (v->playingPreset != -1) {
      f->activeVoiceIndices[writeIndex++] = voiceIndex;
      tsf_link_voice_to_channel(f, voiceIndex);
      tsf_refresh_voice_steal_state(f, voiceIndex);
      continue;
    }
    if (v->pendingFree) {
      v->pendingFree = 0;
      f->freeVoiceIndices[f->freeVoiceCount++] = voiceIndex;
    }
  }
  f->activeVoiceCount = writeIndex;
}

static TSF_BOOL tsf_runtime_has_avx2(void) {
#if defined(_MSC_VER)
  static int initialized = 0;
  static TSF_BOOL hasAVX2 = TSF_FALSE;
  if (!initialized) {
    int cpuInfo[4] = {0, 0, 0, 0};
    __cpuid(cpuInfo, 1);
    if ((cpuInfo[2] & (1 << 27)) && (cpuInfo[2] & (1 << 28))) {
      unsigned long long xcr0 = _xgetbv(0);
      if ((xcr0 & 0x6) == 0x6) {
        __cpuidex(cpuInfo, 7, 0);
        hasAVX2 = (cpuInfo[1] & (1 << 5)) ? TSF_TRUE : TSF_FALSE;
      }
    }
    initialized = 1;
  }
  return hasAVX2;
#else
  return TSF_FALSE;
#endif
}

static float tsf_hsum_ps128(__m128 v) {
  __m128 shuf = _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 3, 0, 1));
  __m128 sums = _mm_add_ps(v, shuf);
  shuf = _mm_movehl_ps(shuf, sums);
  sums = _mm_add_ss(sums, shuf);
  return _mm_cvtss_f32(sums);
}

#if defined(_MSC_VER) || defined(__AVX2__)
static float tsf_hsum_ps256(__m256 v) {
  __m128 low = _mm256_castps256_ps128(v);
  __m128 high = _mm256_extractf128_ps(v, 1);
  return tsf_hsum_ps128(_mm_add_ps(low, high));
}
#endif

static __m128 tsf_blendv_ps_sse2(__m128 a, __m128 b, __m128 mask) {
  return _mm_or_ps(_mm_andnot_ps(mask, a), _mm_and_ps(mask, b));
}

static __m128 tsf_exp2_approx_ps128(__m128 x) {
  const __m128 vMin = _mm_set1_ps(-1.0f);
  const __m128 vMax = _mm_set1_ps(1.0f);
  const __m128 vLn2 = _mm_set1_ps(0.69314718056f);
  const __m128 vC2 = _mm_set1_ps(0.24022650695f);
  const __m128 vC3 = _mm_set1_ps(0.05550410866f);
  __m128 xClamped = _mm_min_ps(vMax, _mm_max_ps(vMin, x));
  __m128 x2 = _mm_mul_ps(xClamped, xClamped);
  __m128 x3 = _mm_mul_ps(x2, xClamped);
  return _mm_add_ps(
      _mm_set1_ps(1.0f),
      _mm_add_ps(_mm_mul_ps(xClamped, vLn2),
                 _mm_add_ps(_mm_mul_ps(x2, vC2), _mm_mul_ps(x3, vC3))));
}

#if defined(_MSC_VER) || defined(__AVX2__)
static __m256 tsf_blendv_ps_avx(__m256 a, __m256 b, __m256 mask) {
  return _mm256_blendv_ps(a, b, mask);
}

static __m256 tsf_exp2_approx_ps256(__m256 x) {
  const __m256 vMin = _mm256_set1_ps(-1.0f);
  const __m256 vMax = _mm256_set1_ps(1.0f);
  const __m256 vLn2 = _mm256_set1_ps(0.69314718056f);
  const __m256 vC2 = _mm256_set1_ps(0.24022650695f);
  const __m256 vC3 = _mm256_set1_ps(0.05550410866f);
  __m256 xClamped = _mm256_min_ps(vMax, _mm256_max_ps(vMin, x));
  __m256 x2 = _mm256_mul_ps(xClamped, xClamped);
  __m256 x3 = _mm256_mul_ps(x2, xClamped);
  return _mm256_add_ps(
      _mm256_set1_ps(1.0f),
      _mm256_add_ps(_mm256_mul_ps(xClamped, vLn2),
                    _mm256_add_ps(_mm256_mul_ps(x2, vC2),
                                  _mm256_mul_ps(x3, vC3))));
}
#endif

static void tsf_voice_step_modenv(tsf *f, int voiceIndex) {
  struct tsf_voice *v = &f->voices[voiceIndex];
  if (f->voiceModEnvSlope[voiceIndex] != 0.0f) {
    if (v->modenv.segmentIsExponential)
      f->voiceModEnvLevel[voiceIndex] *= f->voiceModEnvSlope[voiceIndex];
    else
      f->voiceModEnvLevel[voiceIndex] += f->voiceModEnvSlope[voiceIndex];
  }

  f->voiceModEnvSamplesUntilNextSegment[voiceIndex]--;
  if (f->voiceModEnvSamplesUntilNextSegment[voiceIndex] <= 0) {
    v->modenv.level = f->voiceModEnvLevel[voiceIndex];
    v->modenv.slope = f->voiceModEnvSlope[voiceIndex];
    v->modenv.samplesUntilNextSegment =
        f->voiceModEnvSamplesUntilNextSegment[voiceIndex];
    tsf_voice_envelope_nextsegment(&v->modenv, v->modenv.segment,
                                   f->outSampleRate);
    f->voiceModEnvLevel[voiceIndex] = v->modenv.level;
    f->voiceModEnvSlope[voiceIndex] = v->modenv.slope;
    f->voiceModEnvSamplesUntilNextSegment[voiceIndex] =
        v->modenv.samplesUntilNextSegment;
  }

  v->modenv.level = f->voiceModEnvLevel[voiceIndex];
  v->modenv.slope = f->voiceModEnvSlope[voiceIndex];
  v->modenv.samplesUntilNextSegment =
      f->voiceModEnvSamplesUntilNextSegment[voiceIndex];
}

static void tsf_voice_step_lfo_hot(tsf *f, int voiceIndex, TSF_BOOL vibrato) {
  float *level = (vibrato ? &f->voiceVibLfoLevel[voiceIndex]
                          : &f->voiceModLfoLevel[voiceIndex]);
  float *delta = (vibrato ? &f->voiceVibLfoDelta[voiceIndex]
                          : &f->voiceModLfoDelta[voiceIndex]);
  int *samplesUntil = (vibrato ? &f->voiceVibLfoSamplesUntil[voiceIndex]
                               : &f->voiceModLfoSamplesUntil[voiceIndex]);
  struct tsf_voice_lfo *lfo =
      (vibrato ? &f->voices[voiceIndex].viblfo : &f->voices[voiceIndex].modlfo);

  if (*samplesUntil > 0) {
    --(*samplesUntil);
    if (*samplesUntil > 0) {
      lfo->samplesUntil = *samplesUntil;
      return;
    }
  }

  *level += *delta;
  if (*level > 1.0f) {
    *delta = -*delta;
    *level = 2.0f - *level;
  } else if (*level < -1.0f) {
    *delta = -*delta;
    *level = -2.0f - *level;
  }

  lfo->level = *level;
  lfo->delta = *delta;
  lfo->samplesUntil = *samplesUntil;
}

static void tsf_voice_refresh_filter_hot(tsf *f, int voiceIndex) {
  struct tsf_voice *v = &f->voices[voiceIndex];
  const struct tsf_region *region = v->region;
  float fres;
  float lowpassFc;
  double K, KK, norm;

  if (!region || v->playingPreset == -1) {
    f->voiceFilterActive[voiceIndex] = 0;
    return;
  }

  fres = (float)region->initialFilterFc +
         f->voiceModLfoLevel[voiceIndex] * (float)region->modLfoToFilterFc +
         f->voiceModEnvLevel[voiceIndex] * (float)region->modEnvToFilterFc;
  lowpassFc =
      (fres <= 13500.0f ? tsf_cents2Hertz(fres) / f->outSampleRate : 1.0f);
  f->voiceFilterActive[voiceIndex] = (lowpassFc < 0.499f ? -1 : 0);
  if (!f->voiceFilterActive[voiceIndex])
    return;

  K = TSF_TAN(TSF_PI * lowpassFc);
  KK = K * K;
  norm = 1.0 / (1.0 + K * v->lowpass.QInv + KK);
  f->voiceFilterA0[voiceIndex] = (float)(KK * norm);
  f->voiceFilterA1[voiceIndex] = (float)(2.0 * KK * norm);
  f->voiceFilterB1[voiceIndex] = (float)(2.0 * (KK - 1.0) * norm);
  f->voiceFilterB2[voiceIndex] =
      (float)((1.0 - K * v->lowpass.QInv + KK) * norm);
}

static void tsf_voice_finalize_hot_sample(tsf *f, int voiceIndex) {
  struct tsf_voice *v = &f->voices[voiceIndex];
  if (v->playingPreset == -1)
    return;
  if (v->ampenv.segment == TSF_SEGMENT_DONE) {
    tsf_voice_kill(f, v);
    return;
  }
  if (!(v->loopStart < v->loopEnd) && v->region &&
      f->voiceSourceSamplePosition[voiceIndex] >= (double)v->region->end) {
    tsf_voice_kill(f, v);
    return;
  }
  tsf_voice_sync_meta_from_hot(f, voiceIndex);
}

static void tsf_voice_step_ampenv(tsf *f, int voiceIndex) {
  struct tsf_voice *v = &f->voices[voiceIndex];
  int previousSegment = v->ampenv.segment;
  if (f->voiceAmpEnvSlope[voiceIndex] != 0.0f) {
    if (v->ampenv.segmentIsExponential)
      f->voiceAmpEnvLevel[voiceIndex] *= f->voiceAmpEnvSlope[voiceIndex];
    else
      f->voiceAmpEnvLevel[voiceIndex] += f->voiceAmpEnvSlope[voiceIndex];
  }

  f->voiceAmpEnvSamplesUntilNextSegment[voiceIndex]--;
  if (f->voiceAmpEnvSamplesUntilNextSegment[voiceIndex] <= 0) {
    v->ampenv.level = f->voiceAmpEnvLevel[voiceIndex];
    v->ampenv.slope = f->voiceAmpEnvSlope[voiceIndex];
    v->ampenv.samplesUntilNextSegment =
        f->voiceAmpEnvSamplesUntilNextSegment[voiceIndex];
    tsf_voice_envelope_nextsegment(&v->ampenv, v->ampenv.segment,
                                   f->outSampleRate);
    f->voiceAmpEnvLevel[voiceIndex] = v->ampenv.level;
    f->voiceAmpEnvSlope[voiceIndex] = v->ampenv.slope;
    f->voiceAmpEnvSamplesUntilNextSegment[voiceIndex] =
        v->ampenv.samplesUntilNextSegment;
  }

  v->ampenv.level = f->voiceAmpEnvLevel[voiceIndex];
  v->ampenv.slope = f->voiceAmpEnvSlope[voiceIndex];
  v->ampenv.samplesUntilNextSegment =
      f->voiceAmpEnvSamplesUntilNextSegment[voiceIndex];
  if (v->ampenv.segment == TSF_SEGMENT_DONE)
    f->voiceState[voiceIndex] = TSF_VOICE_STATE_OFF;
  else if (v->ampenv.segment >= TSF_SEGMENT_RELEASE)
    f->voiceState[voiceIndex] = TSF_VOICE_STATE_RELEASE;
  if (v->ampenv.segment != previousSegment)
    tsf_refresh_voice_steal_state(f, voiceIndex);
}

struct tsf_vector_lane {
  struct tsf_voice *voice;
  int voiceIndex;
  TSF_BOOL alive;
  TSF_BOOL isLooping;
  TSF_BOOL updateModEnv;
  TSF_BOOL updateModLFO;
  TSF_BOOL updateVibLFO;
  TSF_BOOL dynamicLowpass;
  double sampleEnd;
  double loopEndPlusOne;
  double loopLength;
  double pos;
  double basePitch;
  float baseVolL;
  float baseVolR;
  float ampLevel;
  float modEnvLevel;
  float modLfoLevel;
  float vibLfoLevel;
  float modLfoToPitch;
  float modEnvToPitch;
  float vibLfoToPitch;
  float modLfoToVolume;
  float initialFilterFc;
  float modLfoToFilterFc;
  float modEnvToFilterFc;
  double pitchRatio;
  float gainL;
  float gainR;
  float filterA0;
  float filterA1;
  float filterB1;
  float filterB2;
  float z1L;
  float z2L;
  float z1R;
  float z2R;
  float filterMask;
};

static TSF_BOOL tsf_batch_lane_init(tsf *f, int voiceIndex,
                                    struct tsf_vector_lane *lane) {
  struct tsf_voice *v;
  struct tsf_region *region;
  TSF_MEMSET(lane, 0, sizeof(*lane));
  lane->voiceIndex = -1;
  if (voiceIndex < 0)
    return TSF_FALSE;

  v = &f->voices[voiceIndex];
  region = v->region;
  if (!region || v->playingPreset == -1 ||
      f->voiceState[voiceIndex] == TSF_VOICE_STATE_OFF)
    return TSF_FALSE;

  lane->voice = v;
  lane->voiceIndex = voiceIndex;
  lane->alive = TSF_TRUE;
  lane->isLooping = (TSF_BOOL)(v->loopStart < v->loopEnd);
  lane->updateModEnv =
      (TSF_BOOL)(region->modEnvToPitch || region->modEnvToFilterFc);
  lane->updateModLFO =
      (TSF_BOOL)(v->modlfo.delta &&
                 (region->modLfoToPitch || region->modLfoToFilterFc ||
                  region->modLfoToVolume));
  lane->updateVibLFO =
      (TSF_BOOL)(v->viblfo.delta && region->vibLfoToPitch);
  lane->dynamicLowpass =
      (TSF_BOOL)(region->modLfoToFilterFc || region->modEnvToFilterFc);
  lane->sampleEnd = (double)region->end;
  lane->loopEndPlusOne = (double)(v->loopEnd + 1);
  lane->loopLength =
      (lane->isLooping ? (double)(v->loopEnd - v->loopStart + 1) : 0.0);
  lane->pos = v->sourceSamplePosition;
  lane->basePitch = f->voiceBasePitchRatio[voiceIndex];
  lane->baseVolL = f->voiceBaseVolumeL[voiceIndex];
  lane->baseVolR = f->voiceBaseVolumeR[voiceIndex];
  lane->ampLevel = f->voiceAmpEnvLevel[voiceIndex];
  lane->modEnvLevel = f->voiceModEnvLevel[voiceIndex];
  lane->modLfoLevel = f->voiceModLfoLevel[voiceIndex];
  lane->vibLfoLevel = f->voiceVibLfoLevel[voiceIndex];
  lane->modLfoToPitch = (float)region->modLfoToPitch;
  lane->modEnvToPitch = (float)region->modEnvToPitch;
  lane->vibLfoToPitch = (float)region->vibLfoToPitch;
  lane->modLfoToVolume = (float)region->modLfoToVolume * 0.1f;
  lane->initialFilterFc = (float)region->initialFilterFc;
  lane->modLfoToFilterFc = (float)region->modLfoToFilterFc;
  lane->modEnvToFilterFc = (float)region->modEnvToFilterFc;
  lane->filterA0 = f->voiceFilterA0[voiceIndex];
  lane->filterA1 = f->voiceFilterA1[voiceIndex];
  lane->filterB1 = f->voiceFilterB1[voiceIndex];
  lane->filterB2 = f->voiceFilterB2[voiceIndex];
  lane->z1L = f->voiceFilterZ1Left[voiceIndex];
  lane->z2L = f->voiceFilterZ2Left[voiceIndex];
  lane->z1R = f->voiceFilterZ1Right[voiceIndex];
  lane->z2R = f->voiceFilterZ2Right[voiceIndex];
  lane->filterMask = (f->voiceFilterActive[voiceIndex] ? -1.0f : 0.0f);

  if (!lane->isLooping && lane->pos >= lane->sampleEnd) {
    tsf_voice_kill(f, v);
    lane->alive = TSF_FALSE;
    return TSF_FALSE;
  }
  return TSF_TRUE;
}

static void tsf_batch_lane_reload_hot(tsf *f, struct tsf_vector_lane *lane) {
  int voiceIndex = lane->voiceIndex;
  if (voiceIndex < 0 || !lane->voice || lane->voice->playingPreset == -1 ||
      f->voiceState[voiceIndex] == TSF_VOICE_STATE_OFF) {
    lane->alive = TSF_FALSE;
    return;
  }
  lane->pos = lane->voice->sourceSamplePosition;
  lane->ampLevel = f->voiceAmpEnvLevel[voiceIndex];
  lane->modEnvLevel = f->voiceModEnvLevel[voiceIndex];
  lane->modLfoLevel = f->voiceModLfoLevel[voiceIndex];
  lane->vibLfoLevel = f->voiceVibLfoLevel[voiceIndex];
  lane->filterA0 = f->voiceFilterA0[voiceIndex];
  lane->filterA1 = f->voiceFilterA1[voiceIndex];
  lane->filterB1 = f->voiceFilterB1[voiceIndex];
  lane->filterB2 = f->voiceFilterB2[voiceIndex];
  lane->z1L = f->voiceFilterZ1Left[voiceIndex];
  lane->z2L = f->voiceFilterZ2Left[voiceIndex];
  lane->z1R = f->voiceFilterZ1Right[voiceIndex];
  lane->z2R = f->voiceFilterZ2Right[voiceIndex];
  lane->filterMask = (f->voiceFilterActive[voiceIndex] ? -1.0f : 0.0f);
}

static TSF_BOOL tsf_batch_lane_prepare_sample(tsf *f,
                                              struct tsf_vector_lane *lane) {
  float modCents;
  float volScale;
  if (!lane->alive || !lane->voice || lane->voice->playingPreset == -1)
    return TSF_FALSE;
  if (!lane->isLooping && lane->pos >= lane->sampleEnd) {
    tsf_voice_kill(f, lane->voice);
    lane->alive = TSF_FALSE;
    return TSF_FALSE;
  }

  if (lane->dynamicLowpass) {
    float fres = lane->initialFilterFc +
                 lane->modLfoLevel * lane->modLfoToFilterFc +
                 lane->modEnvLevel * lane->modEnvToFilterFc;
    float lowpassFc =
        (fres <= 13500.0f ? tsf_cents2Hertz(fres) / f->outSampleRate : 1.0f);
    lane->filterMask = (lowpassFc < 0.499f ? -1.0f : 0.0f);
    if (lane->filterMask != 0.0f) {
      double K = TSF_TAN(TSF_PI * lowpassFc);
      double KK = K * K;
      double norm = 1.0 / (1.0 + K * lane->voice->lowpass.QInv + KK);
      lane->filterA0 = (float)(KK * norm);
      lane->filterA1 = (float)(2.0 * KK * norm);
      lane->filterB1 = (float)(2.0 * (KK - 1.0) * norm);
      lane->filterB2 =
          (float)((1.0 - K * lane->voice->lowpass.QInv + KK) * norm);
    }
  }

  modCents = lane->modLfoLevel * lane->modLfoToPitch +
             lane->vibLfoLevel * lane->vibLfoToPitch +
             lane->modEnvLevel * lane->modEnvToPitch;
  lane->pitchRatio = lane->basePitch * tsf_timecents2Secsd((double)modCents);
  volScale = tsf_decibelsToGain(lane->modLfoLevel * lane->modLfoToVolume);
  lane->gainL = lane->baseVolL * lane->ampLevel * volScale;
  lane->gainR = lane->baseVolR * lane->ampLevel * volScale;
  return TSF_TRUE;
}

static void tsf_batch_lane_finish_sample(tsf *f, struct tsf_vector_lane *lane) {
  int voiceIndex = lane->voiceIndex;
  if (voiceIndex < 0 || !lane->voice || lane->voice->playingPreset == -1) {
    lane->alive = TSF_FALSE;
    return;
  }

  f->voiceSourceSamplePosition[voiceIndex] = lane->pos;
  f->voicePitchRatio[voiceIndex] = lane->pitchRatio;
  f->voiceFilterZ1Left[voiceIndex] = lane->z1L;
  f->voiceFilterZ2Left[voiceIndex] = lane->z2L;
  f->voiceFilterZ1Right[voiceIndex] = lane->z1R;
  f->voiceFilterZ2Right[voiceIndex] = lane->z2R;
  f->voiceFilterA0[voiceIndex] = lane->filterA0;
  f->voiceFilterA1[voiceIndex] = lane->filterA1;
  f->voiceFilterB1[voiceIndex] = lane->filterB1;
  f->voiceFilterB2[voiceIndex] = lane->filterB2;
  f->voiceFilterActive[voiceIndex] = (lane->filterMask != 0.0f ? -1 : 0);

  tsf_voice_step_ampenv(f, voiceIndex);
  if (lane->updateModEnv)
    tsf_voice_step_modenv(f, voiceIndex);
  if (lane->updateModLFO)
    tsf_voice_step_lfo_hot(f, voiceIndex, TSF_FALSE);
  if (lane->updateVibLFO)
    tsf_voice_step_lfo_hot(f, voiceIndex, TSF_TRUE);
  if (lane->dynamicLowpass)
    tsf_voice_refresh_filter_hot(f, voiceIndex);

  tsf_voice_finalize_hot_sample(f, voiceIndex);
  if (lane->voice->playingPreset == -1 ||
      f->voiceState[voiceIndex] == TSF_VOICE_STATE_OFF) {
    lane->alive = TSF_FALSE;
    return;
  }
  lane->alive = TSF_TRUE;
  tsf_batch_lane_reload_hot(f, lane);
}

static void tsf_render_voice_batch_sse2(tsf *f, float *buffer, int samples,
                                        const int *voiceIndices, int voiceCount,
                                        int sampleOffset) {
  int lane, sampleIndex;
  float *input = f->fontSamples;
  struct tsf_vector_lane lanes[4];
  TSF_MEMSET(lanes, 0, sizeof(lanes));

  for (lane = 0; lane < 4; ++lane)
    tsf_batch_lane_init(f, (lane < voiceCount ? voiceIndices[lane] : -1),
                        &lanes[lane]);

  for (sampleIndex = 0; sampleIndex < samples; ++sampleIndex) {
    float alphaArr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float sample0Arr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float sample1Arr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float gainLArr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float gainRArr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float filterA0Arr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float filterA1Arr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float filterB1Arr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float filterB2Arr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float z1LArr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float z2LArr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float z1RArr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float z2RArr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float filterMaskArr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float activeMaskArr[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float mixedLStore[4], mixedRStore[4], z1LStore[4], z2LStore[4], z1RStore[4],
        z2RStore[4];

    for (lane = 0; lane < 4; ++lane) {
      int posIndex, nextPosIndex;
      struct tsf_vector_lane *laneState = &lanes[lane];
      if (!laneState->alive || sampleIndex < sampleOffset ||
          !tsf_batch_lane_prepare_sample(f, laneState))
        continue;
      posIndex = (int)laneState->pos;
      nextPosIndex = (laneState->isLooping && posIndex >= laneState->voice->loopEnd
                          ? (int)laneState->voice->loopStart
                          : posIndex + 1);
      alphaArr[lane] = (float)(laneState->pos - (double)posIndex);
      sample0Arr[lane] = input[posIndex];
      sample1Arr[lane] = input[nextPosIndex];
      gainLArr[lane] = laneState->gainL;
      gainRArr[lane] = laneState->gainR;
      filterA0Arr[lane] = laneState->filterA0;
      filterA1Arr[lane] = laneState->filterA1;
      filterB1Arr[lane] = laneState->filterB1;
      filterB2Arr[lane] = laneState->filterB2;
      z1LArr[lane] = laneState->z1L;
      z2LArr[lane] = laneState->z2L;
      z1RArr[lane] = laneState->z1R;
      z2RArr[lane] = laneState->z2R;
      filterMaskArr[lane] = laneState->filterMask;
      activeMaskArr[lane] = -1.0f;
    }

    {
      __m128 vAlpha = _mm_loadu_ps(alphaArr);
      __m128 vSample0 =
          _mm_set_ps(sample0Arr[3], sample0Arr[2], sample0Arr[1], sample0Arr[0]);
      __m128 vSample1 =
          _mm_set_ps(sample1Arr[3], sample1Arr[2], sample1Arr[1], sample1Arr[0]);
      __m128 vVal = _mm_mul_ps(
          _mm_add_ps(
              _mm_mul_ps(vSample0, _mm_sub_ps(_mm_set1_ps(1.0f), vAlpha)),
              _mm_mul_ps(vSample1, vAlpha)),
          _mm_loadu_ps(activeMaskArr));
      __m128 vInL = _mm_mul_ps(vVal, _mm_loadu_ps(gainLArr));
      __m128 vInR = _mm_mul_ps(vVal, _mm_loadu_ps(gainRArr));
      __m128 vA0 = _mm_loadu_ps(filterA0Arr);
      __m128 vA1 = _mm_loadu_ps(filterA1Arr);
      __m128 vB1 = _mm_loadu_ps(filterB1Arr);
      __m128 vB2 = _mm_loadu_ps(filterB2Arr);
      __m128 vZ1L = _mm_loadu_ps(z1LArr);
      __m128 vZ2L = _mm_loadu_ps(z2LArr);
      __m128 vZ1R = _mm_loadu_ps(z1RArr);
      __m128 vZ2R = _mm_loadu_ps(z2RArr);
      __m128 vFilterMask = _mm_loadu_ps(filterMaskArr);
      __m128 vOutL = _mm_add_ps(_mm_mul_ps(vInL, vA0), vZ1L);
      __m128 vOutR = _mm_add_ps(_mm_mul_ps(vInR, vA0), vZ1R);
      __m128 vNewZ1L =
          _mm_sub_ps(_mm_add_ps(_mm_mul_ps(vInL, vA1), vZ2L),
                     _mm_mul_ps(vB1, vOutL));
      __m128 vNewZ2L =
          _mm_sub_ps(_mm_mul_ps(vInL, vA0), _mm_mul_ps(vB2, vOutL));
      __m128 vNewZ1R =
          _mm_sub_ps(_mm_add_ps(_mm_mul_ps(vInR, vA1), vZ2R),
                     _mm_mul_ps(vB1, vOutR));
      __m128 vNewZ2R =
          _mm_sub_ps(_mm_mul_ps(vInR, vA0), _mm_mul_ps(vB2, vOutR));
      __m128 vMixedL = tsf_blendv_ps_sse2(vInL, vOutL, vFilterMask);
      __m128 vMixedR = tsf_blendv_ps_sse2(vInR, vOutR, vFilterMask);
      __m128 vStoredZ1L = tsf_blendv_ps_sse2(vZ1L, vNewZ1L, vFilterMask);
      __m128 vStoredZ2L = tsf_blendv_ps_sse2(vZ2L, vNewZ2L, vFilterMask);
      __m128 vStoredZ1R = tsf_blendv_ps_sse2(vZ1R, vNewZ1R, vFilterMask);
      __m128 vStoredZ2R = tsf_blendv_ps_sse2(vZ2R, vNewZ2R, vFilterMask);
      _mm_storeu_ps(mixedLStore, vMixedL);
      _mm_storeu_ps(mixedRStore, vMixedR);
      _mm_storeu_ps(z1LStore, vStoredZ1L);
      _mm_storeu_ps(z2LStore, vStoredZ2L);
      _mm_storeu_ps(z1RStore, vStoredZ1R);
      _mm_storeu_ps(z2RStore, vStoredZ2R);
    }

    buffer[sampleIndex * 2] +=
        mixedLStore[0] + mixedLStore[1] + mixedLStore[2] + mixedLStore[3];
    buffer[sampleIndex * 2 + 1] +=
        mixedRStore[0] + mixedRStore[1] + mixedRStore[2] + mixedRStore[3];

    for (lane = 0; lane < 4; ++lane) {
      struct tsf_vector_lane *laneState = &lanes[lane];
      if (activeMaskArr[lane] == 0.0f)
        continue;
      laneState->z1L = z1LStore[lane];
      laneState->z2L = z2LStore[lane];
      laneState->z1R = z1RStore[lane];
      laneState->z2R = z2RStore[lane];
      laneState->pos += (double)laneState->pitchRatio;
      if (laneState->isLooping && laneState->pos >= laneState->loopEndPlusOne)
        laneState->pos -= laneState->loopLength;
      tsf_batch_lane_finish_sample(f, laneState);
    }

    if (sampleIndex + 1 >= sampleOffset) {
      TSF_BOOL anyAlive = TSF_FALSE;
      for (lane = 0; lane < 4; ++lane)
        if (lanes[lane].alive) {
          anyAlive = TSF_TRUE;
          break;
        }
      if (!anyAlive)
        break;
    }
  }
}

#if defined(_MSC_VER) || defined(__AVX2__)
static void tsf_render_voice_batch_avx2(tsf *f, float *buffer, int samples,
                                        const int *voiceIndices, int voiceCount,
                                        int sampleOffset) {
  int lane, sampleIndex;
  float *input = f->fontSamples;
  struct tsf_vector_lane lanes[8];
  TSF_MEMSET(lanes, 0, sizeof(lanes));

  for (lane = 0; lane < 8; ++lane)
    tsf_batch_lane_init(f, (lane < voiceCount ? voiceIndices[lane] : -1),
                        &lanes[lane]);

  for (sampleIndex = 0; sampleIndex < samples; ++sampleIndex) {
    int posIndexArr[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int nextPosIndexArr[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    float alphaArr[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float gainLArr[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float gainRArr[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float filterA0Arr[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float filterA1Arr[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float filterB1Arr[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float filterB2Arr[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float z1LArr[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float z2LArr[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float z1RArr[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float z2RArr[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float filterMaskArr[8] = {0.0f, 0.0f, 0.0f, 0.0f,
                              0.0f, 0.0f, 0.0f, 0.0f};
    float activeMaskArr[8] = {0.0f, 0.0f, 0.0f, 0.0f,
                              0.0f, 0.0f, 0.0f, 0.0f};
    float mixedLStore[8], mixedRStore[8], z1LStore[8], z2LStore[8], z1RStore[8],
        z2RStore[8];

    for (lane = 0; lane < 8; ++lane) {
      int posIndex, nextPosIndex;
      struct tsf_vector_lane *laneState = &lanes[lane];
      if (!laneState->alive || sampleIndex < sampleOffset ||
          !tsf_batch_lane_prepare_sample(f, laneState))
        continue;
      posIndex = (int)laneState->pos;
      nextPosIndex = (laneState->isLooping && posIndex >= laneState->voice->loopEnd
                          ? (int)laneState->voice->loopStart
                          : posIndex + 1);
      posIndexArr[lane] = posIndex;
      nextPosIndexArr[lane] = nextPosIndex;
      alphaArr[lane] = (float)(laneState->pos - (double)posIndex);
      gainLArr[lane] = laneState->gainL;
      gainRArr[lane] = laneState->gainR;
      filterA0Arr[lane] = laneState->filterA0;
      filterA1Arr[lane] = laneState->filterA1;
      filterB1Arr[lane] = laneState->filterB1;
      filterB2Arr[lane] = laneState->filterB2;
      z1LArr[lane] = laneState->z1L;
      z2LArr[lane] = laneState->z2L;
      z1RArr[lane] = laneState->z1R;
      z2RArr[lane] = laneState->z2R;
      filterMaskArr[lane] = laneState->filterMask;
      activeMaskArr[lane] = -1.0f;
    }

    {
      __m256 vAlpha = _mm256_loadu_ps(alphaArr);
      __m256 vOneMinusAlpha = _mm256_sub_ps(_mm256_set1_ps(1.0f), vAlpha);
      __m256 vSample0 = _mm256_i32gather_ps(
          input, _mm256_loadu_si256((const __m256i *)posIndexArr), 4);
      __m256 vSample1 = _mm256_i32gather_ps(
          input, _mm256_loadu_si256((const __m256i *)nextPosIndexArr), 4);
      __m256 vVal = _mm256_mul_ps(
          _mm256_add_ps(_mm256_mul_ps(vSample0, vOneMinusAlpha),
                        _mm256_mul_ps(vSample1, vAlpha)),
          _mm256_loadu_ps(activeMaskArr));
      __m256 vInL = _mm256_mul_ps(vVal, _mm256_loadu_ps(gainLArr));
      __m256 vInR = _mm256_mul_ps(vVal, _mm256_loadu_ps(gainRArr));
      __m256 vA0 = _mm256_loadu_ps(filterA0Arr);
      __m256 vA1 = _mm256_loadu_ps(filterA1Arr);
      __m256 vB1 = _mm256_loadu_ps(filterB1Arr);
      __m256 vB2 = _mm256_loadu_ps(filterB2Arr);
      __m256 vZ1L = _mm256_loadu_ps(z1LArr);
      __m256 vZ2L = _mm256_loadu_ps(z2LArr);
      __m256 vZ1R = _mm256_loadu_ps(z1RArr);
      __m256 vZ2R = _mm256_loadu_ps(z2RArr);
      __m256 vFilterMask = _mm256_loadu_ps(filterMaskArr);
      __m256 vOutL = _mm256_add_ps(_mm256_mul_ps(vInL, vA0), vZ1L);
      __m256 vOutR = _mm256_add_ps(_mm256_mul_ps(vInR, vA0), vZ1R);
      __m256 vNewZ1L =
          _mm256_sub_ps(_mm256_add_ps(_mm256_mul_ps(vInL, vA1), vZ2L),
                        _mm256_mul_ps(vB1, vOutL));
      __m256 vNewZ2L =
          _mm256_sub_ps(_mm256_mul_ps(vInL, vA0), _mm256_mul_ps(vB2, vOutL));
      __m256 vNewZ1R =
          _mm256_sub_ps(_mm256_add_ps(_mm256_mul_ps(vInR, vA1), vZ2R),
                        _mm256_mul_ps(vB1, vOutR));
      __m256 vNewZ2R =
          _mm256_sub_ps(_mm256_mul_ps(vInR, vA0), _mm256_mul_ps(vB2, vOutR));
      __m256 vMixedL = tsf_blendv_ps_avx(vInL, vOutL, vFilterMask);
      __m256 vMixedR = tsf_blendv_ps_avx(vInR, vOutR, vFilterMask);
      __m256 vStoredZ1L = tsf_blendv_ps_avx(vZ1L, vNewZ1L, vFilterMask);
      __m256 vStoredZ2L = tsf_blendv_ps_avx(vZ2L, vNewZ2L, vFilterMask);
      __m256 vStoredZ1R = tsf_blendv_ps_avx(vZ1R, vNewZ1R, vFilterMask);
      __m256 vStoredZ2R = tsf_blendv_ps_avx(vZ2R, vNewZ2R, vFilterMask);
      _mm256_storeu_ps(mixedLStore, vMixedL);
      _mm256_storeu_ps(mixedRStore, vMixedR);
      _mm256_storeu_ps(z1LStore, vStoredZ1L);
      _mm256_storeu_ps(z2LStore, vStoredZ2L);
      _mm256_storeu_ps(z1RStore, vStoredZ1R);
      _mm256_storeu_ps(z2RStore, vStoredZ2R);
    }

    buffer[sampleIndex * 2] += tsf_hsum_ps256(_mm256_loadu_ps(mixedLStore));
    buffer[sampleIndex * 2 + 1] +=
        tsf_hsum_ps256(_mm256_loadu_ps(mixedRStore));

    for (lane = 0; lane < 8; ++lane) {
      struct tsf_vector_lane *laneState = &lanes[lane];
      if (activeMaskArr[lane] == 0.0f)
        continue;
      laneState->z1L = z1LStore[lane];
      laneState->z2L = z2LStore[lane];
      laneState->z1R = z1RStore[lane];
      laneState->z2R = z2RStore[lane];
      laneState->pos += (double)laneState->pitchRatio;
      if (laneState->isLooping && laneState->pos >= laneState->loopEndPlusOne)
        laneState->pos -= laneState->loopLength;
      tsf_batch_lane_finish_sample(f, laneState);
    }

    if (sampleIndex + 1 >= sampleOffset) {
      TSF_BOOL anyAlive = TSF_FALSE;
      for (lane = 0; lane < 8; ++lane)
        if (lanes[lane].alive) {
          anyAlive = TSF_TRUE;
          break;
        }
      if (!anyAlive)
        break;
    }
  }
  _mm256_zeroupper();
}
#endif

static void tsf_render_float_scalar_indexed(tsf *f, float *buffer, int samples,
                                            int flag_mixing,
                                            const int *voice_indices,
                                            int voice_count,
                                            int sampleOffset) {
  int i;
  int channels = (f->outputmode == TSF_MONO ? 1 : 2);
  if (sampleOffset < 0)
    sampleOffset = 0;
  if (!flag_mixing)
    TSF_MEMSET(buffer, 0, (size_t)channels * sizeof(float) * samples);
  if (voice_count <= 0 || sampleOffset >= samples)
    return;

  {
    int renderSamples = samples - sampleOffset;
    if (renderSamples <= 0)
      return;
    if (f->outputmode == TSF_STEREO_UNWEAVED && sampleOffset > 0) {
      float *temp =
          (float *)TSF_MALLOC((size_t)renderSamples * sizeof(float) * 2);
      if (!temp)
        return;
      TSF_MEMSET(temp, 0, (size_t)renderSamples * sizeof(float) * 2);
      for (i = 0; i < voice_count; ++i) {
        struct tsf_voice *v = &f->voices[voice_indices[i]];
        if (v->playingPreset != -1)
          tsf_voice_render(f, v, temp, renderSamples);
      }
      TSF_MEMCPY(buffer + sampleOffset, temp, sizeof(float) * renderSamples);
      TSF_MEMCPY(buffer + samples + sampleOffset, temp + renderSamples,
                 sizeof(float) * renderSamples);
      TSF_FREE(temp);
      return;
    }

    {
      float *target = buffer + sampleOffset * channels;
      for (i = 0; i < voice_count; ++i) {
        struct tsf_voice *v = &f->voices[voice_indices[i]];
        if (v->playingPreset != -1)
          tsf_voice_render(f, v, target, renderSamples);
      }
    }
  }
}

static void tsf_render_float_vectorized(tsf *f, float *buffer, int samples,
                                        int flag_mixing,
                                        const int *voice_indices,
                                        int voice_count, int sampleOffset) {
  tsf_render_float_scalar_indexed(f, buffer, samples, flag_mixing,
                                  voice_indices, voice_count, sampleOffset);
}

TSFDEF void tsf_render_float_indexed(tsf *f, float *buffer, int samples,
                                     int flag_mixing,
                                     const int *voice_indices,
                                     int voice_count) {
  if (samples < TSF_RENDER_MIN_SIMD_SAMPLES)
    tsf_render_float_scalar_indexed(f, buffer, samples, flag_mixing,
                                    voice_indices, voice_count, 0);
  else
    tsf_render_float_vectorized(f, buffer, samples, flag_mixing, voice_indices,
                                voice_count, 0);
}

TSFDEF void tsf_render_short(tsf *f, short *buffer, int samples,
                             int flag_mixing) {
  float outputSamples[TSF_RENDER_SHORTBUFFERBLOCK];
  int channels = (f->outputmode == TSF_MONO ? 1 : 2),
      maxChannelSamples = TSF_RENDER_SHORTBUFFERBLOCK / channels;
  while (samples > 0) {
    int channelSamples =
        (samples > maxChannelSamples ? maxChannelSamples : samples);
    short *bufferEnd = buffer + channelSamples * channels;
    float *floatSamples = outputSamples;
    tsf_render_float(f, floatSamples, channelSamples, TSF_FALSE);
    samples -= channelSamples;

    if (flag_mixing)
      while (buffer != bufferEnd) {
        float v = *floatSamples++;
        int vi = *buffer +
                 (v < -1.00004566f
                      ? (int)-32768
                      : (v > 1.00001514f ? (int)32767 : (int)(v * 32767.5f)));
        *buffer++ = (vi < -32768 ? (short)-32768
                                 : (vi > 32767 ? (short)32767 : (short)vi));
      }
    else
      while (buffer != bufferEnd) {
        float v = *floatSamples++;
        *buffer++ =
            (v < -1.00004566f
                 ? (short)-32768
                 : (v > 1.00001514f ? (short)32767 : (short)(v * 32767.5f)));
      }
  }
}

TSFDEF void tsf_render_float(tsf *f, float *buffer, int samples,
                             int flag_mixing) {
  tsf_render_float_indexed(f, buffer, samples, flag_mixing,
                           f->activeVoiceIndices, f->activeVoiceCount);
  tsf_cleanup_inactive_voices(f);
}

static void tsf_channel_setup_voice(tsf *f, struct tsf_voice *v) {
  struct tsf_channel *c = &f->channels->channels[f->channels->activeChannel];
  float newpan = v->region->pan + c->panOffset;
  v->playingChannel = f->channels->activeChannel;
  v->noteGainDB += c->gainDB;
  tsf_voice_calcpitchratio(
      v,
      (c->pitchWheel == 8192
           ? c->tuning
           : ((c->pitchWheel / 16383.0f * c->pitchRange * 2.0f) -
              c->pitchRange + c->tuning)),
      f->outSampleRate);
  if (newpan <= -0.5f) {
    v->panFactorLeft = 1.0f;
    v->panFactorRight = 0.0f;
  } else if (newpan >= 0.5f) {
    v->panFactorLeft = 0.0f;
    v->panFactorRight = 1.0f;
  } else {
    v->panFactorLeft = TSF_SQRTF(0.5f - newpan);
    v->panFactorRight = TSF_SQRTF(0.5f + newpan);
  }
  tsf_voice_update_hot_state(f, tsf_voice_index(f, v));
}

static struct tsf_channel *tsf_channel_init(tsf *f, int channel) {
  int i;
  if (f->channels && channel < f->channels->channelNum)
    return &f->channels->channels[channel];
  if (!f->channels) {
    f->channels = (struct tsf_channels *)TSF_MALLOC(
        sizeof(struct tsf_channels) + sizeof(struct tsf_channel) * channel);
    if (!f->channels)
      return TSF_NULL;
    f->channels->setupVoice = &tsf_channel_setup_voice;
    f->channels->channelNum = 0;
    f->channels->activeChannel = 0;
  } else {
    struct tsf_channels *newChannels = (struct tsf_channels *)TSF_REALLOC(
        f->channels,
        sizeof(struct tsf_channels) + sizeof(struct tsf_channel) * channel);
    if (!newChannels)
      return TSF_NULL;
    f->channels = newChannels;
  }
  i = f->channels->channelNum;
  f->channels->channelNum = channel + 1;
  for (; i <= channel; i++) {
    struct tsf_channel *c = &f->channels->channels[i];
    c->presetIndex = c->bank = 0;
    c->pitchWheel = c->midiPan = 8192;
    c->midiVolume = c->midiExpression = 16383;
    c->midiRPN = 0xFFFF;
    c->midiData = c->sustain = 0;
    c->panOffset = 0.0f;
    c->gainDB = 0.0f;
    c->pitchRange = 2.0f;
    c->tuning = 0.0f;
  }
  return &f->channels->channels[channel];
}

static void tsf_channel_applypitch(tsf *f, int channel, struct tsf_channel *c) {
  float pitchShift = (c->pitchWheel == 8192
                          ? c->tuning
                          : ((c->pitchWheel / 16383.0f * c->pitchRange * 2.0f) -
                             c->pitchRange + c->tuning));
  int voiceIndex;
  tsf_ensure_channel_voice_cache(f);
  for (voiceIndex = (channel >= 0 && channel < 16 ? f->channelVoiceHeads[channel]
                                                   : -1);
       voiceIndex != -1;
       voiceIndex = f->voices[voiceIndex].nextChannelVoice) {
    struct tsf_voice *v = &f->voices[voiceIndex];
    if (v->playingPreset != -1 && v->playingChannel == channel)
      tsf_voice_calcpitchratio(v, pitchShift, f->outSampleRate),
          tsf_voice_update_hot_state(f, voiceIndex);
  }
}

TSFDEF int tsf_channel_set_presetindex(tsf *f, int channel, int preset_index) {
  struct tsf_channel *c = tsf_channel_init(f, channel);
  if (!c)
    return 0;
  c->presetIndex = (unsigned short)preset_index;
  return 1;
}

TSFDEF int tsf_channel_set_presetnumber(tsf *f, int channel, int preset_number,
                                        int flag_mididrums) {
  int preset_index;
  struct tsf_channel *c = tsf_channel_init(f, channel);
  if (!c)
    return 0;
  if (flag_mididrums) {
    preset_index =
        tsf_get_presetindex(f, 128 | (c->bank & 0x7FFF), preset_number);
    if (preset_index == -1)
      preset_index = tsf_get_presetindex(f, 128, preset_number);
    if (preset_index == -1)
      preset_index = tsf_get_presetindex(f, 128, 0);
    if (preset_index == -1)
      preset_index = tsf_get_presetindex(f, (c->bank & 0x7FFF), preset_number);
  } else
    preset_index = tsf_get_presetindex(f, (c->bank & 0x7FFF), preset_number);
  if (preset_index == -1)
    preset_index = tsf_get_presetindex(f, 0, preset_number);
  if (preset_index != -1) {
    c->presetIndex = (unsigned short)preset_index;
    return 1;
  }
  return 0;
}

TSFDEF int tsf_channel_set_bank(tsf *f, int channel, int bank) {
  struct tsf_channel *c = tsf_channel_init(f, channel);
  if (!c)
    return 0;
  c->bank = (unsigned short)bank;
  return 1;
}

TSFDEF int tsf_channel_set_bank_preset(tsf *f, int channel, int bank,
                                       int preset_number) {
  int preset_index;
  struct tsf_channel *c = tsf_channel_init(f, channel);
  if (!c)
    return 0;
  preset_index = tsf_get_presetindex(f, bank, preset_number);
  if (preset_index == -1)
    return 0;
  c->presetIndex = (unsigned short)preset_index;
  c->bank = (unsigned short)bank;
  return 1;
}

TSFDEF int tsf_channel_set_pan(tsf *f, int channel, float pan) {
  struct tsf_channel *c = tsf_channel_init(f, channel);
  int voiceIndex;
  if (!c)
    return 0;
  tsf_ensure_channel_voice_cache(f);
  for (voiceIndex = (channel >= 0 && channel < 16 ? f->channelVoiceHeads[channel]
                                                   : -1);
       voiceIndex != -1;
       voiceIndex = f->voices[voiceIndex].nextChannelVoice) {
    struct tsf_voice *v = &f->voices[voiceIndex];
    if (v->playingPreset != -1 && v->playingChannel == channel) {
      float newpan = v->region->pan + pan - 0.5f;
      if (newpan <= -0.5f) {
        v->panFactorLeft = 1.0f;
        v->panFactorRight = 0.0f;
      } else if (newpan >= 0.5f) {
        v->panFactorLeft = 0.0f;
        v->panFactorRight = 1.0f;
      } else {
        v->panFactorLeft = TSF_SQRTF(0.5f - newpan);
        v->panFactorRight = TSF_SQRTF(0.5f + newpan);
      }
      tsf_voice_update_hot_state(f, voiceIndex);
    }
  }
  c->panOffset = pan - 0.5f;
  return 1;
}

TSFDEF int tsf_channel_set_volume(tsf *f, int channel, float volume) {
  float gainDB = tsf_gainToDecibels(volume), gainDBChange;
  struct tsf_channel *c = tsf_channel_init(f, channel);
  int voiceIndex;
  if (!c)
    return 0;
  if (gainDB == c->gainDB)
    return 1;
  gainDBChange = gainDB - c->gainDB;
  tsf_ensure_channel_voice_cache(f);
  for (voiceIndex = (channel >= 0 && channel < 16 ? f->channelVoiceHeads[channel]
                                                   : -1);
       voiceIndex != -1;
       voiceIndex = f->voices[voiceIndex].nextChannelVoice) {
    struct tsf_voice *v = &f->voices[voiceIndex];
    if (v->playingPreset != -1 && v->playingChannel == channel)
      v->noteGainDB += gainDBChange, tsf_voice_update_hot_state(f, voiceIndex);
  }
  c->gainDB = gainDB;
  return 1;
}

TSFDEF int tsf_channel_set_pitchwheel(tsf *f, int channel, int pitch_wheel) {
  struct tsf_channel *c = tsf_channel_init(f, channel);
  if (!c)
    return 0;
  if (c->pitchWheel == pitch_wheel)
    return 1;
  c->pitchWheel = (unsigned short)pitch_wheel;
  tsf_channel_applypitch(f, channel, c);
  return 1;
}

TSFDEF int tsf_channel_set_pitchrange(tsf *f, int channel, float pitch_range) {
  struct tsf_channel *c = tsf_channel_init(f, channel);
  if (!c)
    return 0;
  if (c->pitchRange == pitch_range)
    return 1;
  c->pitchRange = pitch_range;
  if (c->pitchWheel != 8192)
    tsf_channel_applypitch(f, channel, c);
  return 1;
}

TSFDEF int tsf_channel_set_tuning(tsf *f, int channel, float tuning) {
  struct tsf_channel *c = tsf_channel_init(f, channel);
  if (!c)
    return 0;
  if (c->tuning == tuning)
    return 1;
  c->tuning = tuning;
  tsf_channel_applypitch(f, channel, c);
  return 1;
}

TSFDEF int tsf_channel_set_sustain(tsf *f, int channel, int flag_sustain) {
  struct tsf_channel *c = tsf_channel_init(f, channel);
  if (!c)
    return 0;
  if (!c->sustain == !flag_sustain)
    return 1;
  c->sustain = (unsigned short)(flag_sustain != 0);
  // Turning on sustain does no action now, just starts note_off behaving
  // differently
  if (flag_sustain)
    return 1;
  // Turning off sustain, actually end voices that got a note_off and were set
  // to heldSustain status
  int voiceIndex;
  tsf_ensure_channel_voice_cache(f);
  for (voiceIndex = (channel >= 0 && channel < 16 ? f->channelVoiceHeads[channel]
                                                   : -1);
       voiceIndex != -1;
       voiceIndex = f->voices[voiceIndex].nextChannelVoice) {
    struct tsf_voice *v = &f->voices[voiceIndex];
    if (v->playingPreset != -1 && v->playingChannel == channel &&
        v->ampenv.segment < TSF_SEGMENT_RELEASE && v->heldSustain)
      tsf_voice_end(f, v);
  }
  return 1;
}

TSFDEF int tsf_channel_note_on(tsf *f, int channel, int key, float vel) {
  return tsf_channel_note_on_ex(f, channel, key, vel, (int)(vel * 127.0f));
}

TSFDEF int tsf_channel_note_on_ex(tsf *f, int channel, int key, float gain_vel,
                                  int midi_velocity) {
  if (!f->channels || channel >= f->channels->channelNum)
    return 1;
  f->channels->activeChannel = channel;
  if (!gain_vel) {
    tsf_channel_note_off(f, channel, key);
    return 1;
  }
  return tsf_note_on_ex(f, f->channels->channels[channel].presetIndex, key,
                        gain_vel, midi_velocity);
}

TSFDEF void tsf_channel_note_off(tsf *f, int channel, int key) {
  unsigned sustain;
  unsigned int matchPlayIndex = 0;
  TSF_BOOL found = TSF_FALSE;
  int voiceIndex;
  if (channel < 0 || channel >= 16 || key < 0 || key >= 128)
    return;
  tsf_ensure_channel_voice_cache(f);
  for (voiceIndex = f->channelKeyVoiceHeads[channel][key];
       voiceIndex != -1;
       voiceIndex = f->voices[voiceIndex].nextChannelKeyVoice) {
    struct tsf_voice *v = &f->voices[voiceIndex];
    if (v->playingPreset == -1 || v->playingChannel != channel ||
        v->ampenv.segment >= TSF_SEGMENT_RELEASE || v->heldSustain)
      continue;
    if (!found || v->playIndex < matchPlayIndex) {
      matchPlayIndex = v->playIndex;
      found = TSF_TRUE;
    }
  }
  if (!found)
    return;
  for (sustain = f->channels->channels[channel].sustain,
      voiceIndex = f->channelKeyVoiceHeads[channel][key];
       voiceIndex != -1;
       voiceIndex = f->voices[voiceIndex].nextChannelKeyVoice) {
    struct tsf_voice *v = &f->voices[voiceIndex];
    if (v->playingPreset == -1 || v->playingChannel != channel ||
        v->playIndex != matchPlayIndex || v->ampenv.segment >= TSF_SEGMENT_RELEASE ||
        v->heldSustain)
      continue;
    // Don't turn off if sustain is active, just mark as held by sustain so we
    // don't forget it
    if (sustain)
      v->heldSustain = 1;
    else
      tsf_voice_end(f, v);
  }
}

TSFDEF void tsf_channel_note_off_all(tsf *f, int channel) {
  // Ignore sustain channel settings, note_off_all overrides
  int voiceIndex;
  tsf_ensure_channel_voice_cache(f);
  for (voiceIndex = (channel >= 0 && channel < 16 ? f->channelVoiceHeads[channel]
                                                   : -1);
       voiceIndex != -1;
       voiceIndex = f->voices[voiceIndex].nextChannelVoice) {
    struct tsf_voice *v = &f->voices[voiceIndex];
    if (v->playingPreset != -1 && v->playingChannel == channel &&
        v->ampenv.segment < TSF_SEGMENT_RELEASE)
      tsf_voice_end(f, v);
  }
}

TSFDEF void tsf_channel_sounds_off_all(tsf *f, int channel) {
  int voiceIndex;
  tsf_ensure_channel_voice_cache(f);
  for (voiceIndex = (channel >= 0 && channel < 16 ? f->channelVoiceHeads[channel]
                                                   : -1);
       voiceIndex != -1;
       voiceIndex = f->voices[voiceIndex].nextChannelVoice) {
    struct tsf_voice *v = &f->voices[voiceIndex];
    if (v->playingPreset != -1 && v->playingChannel == channel &&
        (v->ampenv.segment < TSF_SEGMENT_RELEASE ||
         v->ampenv.parameters.release))
      tsf_voice_endquick(f, v);
  }
}

TSFDEF int tsf_channel_midi_control(tsf *f, int channel, int controller,
                                    int control_value) {
  struct tsf_channel *c = tsf_channel_init(f, channel);
  if (!c)
    return 0;
  switch (controller) {
  case 7 /*VOLUME_MSB*/:
    c->midiVolume =
        (unsigned short)((c->midiVolume & 0x7F) | (control_value << 7));
    goto TCMC_SET_VOLUME;
  case 39 /*VOLUME_LSB*/:
    c->midiVolume = (unsigned short)((c->midiVolume & 0x3F80) | control_value);
    goto TCMC_SET_VOLUME;
  case 11 /*EXPRESSION_MSB*/:
    c->midiExpression =
        (unsigned short)((c->midiExpression & 0x7F) | (control_value << 7));
    goto TCMC_SET_VOLUME;
  case 43 /*EXPRESSION_LSB*/:
    c->midiExpression =
        (unsigned short)((c->midiExpression & 0x3F80) | control_value);
    goto TCMC_SET_VOLUME;
  case 10 /*PAN_MSB*/:
    c->midiPan = (unsigned short)((c->midiPan & 0x7F) | (control_value << 7));
    goto TCMC_SET_PAN;
  case 42 /*PAN_LSB*/:
    c->midiPan = (unsigned short)((c->midiPan & 0x3F80) | control_value);
    goto TCMC_SET_PAN;
  case 6 /*DATA_ENTRY_MSB*/:
    c->midiData = (unsigned short)((c->midiData & 0x7F) | (control_value << 7));
    goto TCMC_SET_DATA;
  case 38 /*DATA_ENTRY_LSB*/:
    c->midiData = (unsigned short)((c->midiData & 0x3F80) | control_value);
    goto TCMC_SET_DATA;
  case 0 /*BANK_SELECT_MSB*/:
    c->bank = (unsigned short)(0x8000 | control_value);
    return 1; // bank select MSB alone acts like LSB
  case 32 /*BANK_SELECT_LSB*/:
    c->bank =
        (unsigned short)((c->bank & 0x8000 ? ((c->bank & 0x7F) << 7) : 0) |
                         control_value);
    return 1;
  case 101 /*RPN_MSB*/:
    c->midiRPN =
        (unsigned short)(((c->midiRPN == 0xFFFF ? 0 : c->midiRPN) & 0x7F) |
                         (control_value << 7));
    return 1;
  case 100 /*RPN_LSB*/:
    c->midiRPN =
        (unsigned short)(((c->midiRPN == 0xFFFF ? 0 : c->midiRPN) & 0x3F80) |
                         control_value);
    return 1;
  case 98 /*NRPN_LSB*/:
    c->midiRPN = 0xFFFF;
    return 1;
  case 99 /*NRPN_MSB*/:
    c->midiRPN = 0xFFFF;
    return 1;
  case 64 /*SUSTAIN*/:
    tsf_channel_set_sustain(f, channel, (int)(control_value >= 64));
    return 1;
  case 120 /*ALL_SOUND_OFF*/:
    tsf_channel_sounds_off_all(f, channel);
    return 1;
  case 123 /*ALL_NOTES_OFF*/:
    tsf_channel_note_off_all(f, channel);
    return 1;
  case 121 /*ALL_CTRL_OFF*/:
    c->midiVolume = c->midiExpression = 16383;
    c->midiPan = 8192;
    c->bank = 0;
    c->midiRPN = 0xFFFF;
    c->midiData = 0;
    tsf_channel_set_volume(f, channel, 1.0f);
    tsf_channel_set_pan(f, channel, 0.5f);
    tsf_channel_set_pitchrange(f, channel, 2.0f);
    tsf_channel_set_tuning(f, channel, 0);
    return 1;
  }
  return 1;
TCMC_SET_VOLUME:
  // Raising to the power of 3 seems to result in a decent sounding volume curve
  // for MIDI
  tsf_channel_set_volume(
      f, channel,
      TSF_POWF((c->midiVolume / 16383.0f) * (c->midiExpression / 16383.0f),
               3.0f));
  return 1;
TCMC_SET_PAN:
  tsf_channel_set_pan(f, channel, c->midiPan / 16383.0f);
  return 1;
TCMC_SET_DATA:
  if (c->midiRPN == 0)
    tsf_channel_set_pitchrange(
        f, channel, (c->midiData >> 7) + 0.01f * (c->midiData & 0x7F));
  else if (c->midiRPN == 1)
    tsf_channel_set_tuning(f, channel,
                           (int)c->tuning + ((float)c->midiData - 8192.0f) /
                                                8192.0f); // fine tune
  else if (c->midiRPN == 2 && controller == 6)
    tsf_channel_set_tuning(f, channel,
                           ((float)control_value - 64.0f) +
                               (c->tuning - (int)c->tuning)); // coarse tune
  return 1;
}

TSFDEF int tsf_channel_get_preset_index(tsf *f, int channel) {
  return (f->channels && channel < f->channels->channelNum
              ? f->channels->channels[channel].presetIndex
              : 0);
}

TSFDEF int tsf_channel_get_preset_bank(tsf *f, int channel) {
  return (f->channels && channel < f->channels->channelNum
              ? (f->channels->channels[channel].bank & 0x7FFF)
              : 0);
}

TSFDEF int tsf_channel_get_preset_number(tsf *f, int channel) {
  return (f->channels && channel < f->channels->channelNum
              ? f->presets[f->channels->channels[channel].presetIndex].preset
              : 0);
}

TSFDEF float tsf_channel_get_pan(tsf *f, int channel) {
  return (f->channels && channel < f->channels->channelNum
              ? f->channels->channels[channel].panOffset - 0.5f
              : 0.5f);
}

TSFDEF float tsf_channel_get_volume(tsf *f, int channel) {
  return (f->channels && channel < f->channels->channelNum
              ? tsf_decibelsToGain(f->channels->channels[channel].gainDB)
              : 1.0f);
}

TSFDEF int tsf_channel_get_pitchwheel(tsf *f, int channel) {
  return (f->channels && channel < f->channels->channelNum
              ? f->channels->channels[channel].pitchWheel
              : 8192);
}

TSFDEF float tsf_channel_get_pitchrange(tsf *f, int channel) {
  return (f->channels && channel < f->channels->channelNum
              ? f->channels->channels[channel].pitchRange
              : 2.0f);
}

TSFDEF float tsf_channel_get_tuning(tsf *f, int channel) {
  return (f->channels && channel < f->channels->channelNum
              ? f->channels->channels[channel].tuning
              : 0.0f);
}

#ifdef __cplusplus
}
#endif

#endif // TSF_IMPLEMENTATION
