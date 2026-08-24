#ifndef SVMS_SYSEX_H
#define SVMS_SYSEX_H

#include "SVMSEventCompile.h"

#include <cstdint>

namespace svms {

inline uint32_t MakeMidiControlChange(uint8_t channel, uint8_t controller,
                                      uint8_t value) noexcept {
    return static_cast<uint32_t>(0xb0u | (channel & 0x0fu)) |
        (static_cast<uint32_t>(controller & 0x7fu) << 8u) |
        (static_cast<uint32_t>(value & 0x7fu) << 16u);
}

inline uint32_t MakeMidiProgramChange(uint8_t channel,
                                      uint8_t program) noexcept {
    return static_cast<uint32_t>(0xc0u | (channel & 0x0fu)) |
        (static_cast<uint32_t>(program & 0x7fu) << 8u);
}

// Translate an SMF SysEx payload. The initial F0 byte is not stored in an SMF
// event, while the terminating F7 normally is.
template <typename Emit>
bool TranslateXGSystemExclusivePayload(const uint8_t* data, uint32_t size,
                                       Emit&& emit) {
    if (!data || size < 8u || data[size - 1u] != 0xf7u ||
        data[0] != 0x43u || data[2] != 0x4cu) {
        return false;
    }

    const uint8_t addressHigh = data[3] & 0x7fu;
    const uint8_t addressMiddle = data[4] & 0x7fu;
    const uint8_t addressLow = data[5] & 0x7fu;
    const uint32_t payloadSize = size - 7u;
    const uint8_t* payload = data + 6u;

    if (addressHigh == 0x00u && addressMiddle == 0x00u) {
        if ((addressLow == 0x7eu || addressLow == 0x7fu) &&
            payload[0] == 0u) {
            emit(kInternalResetMessage);
            return true;
        }
        if (addressLow == 0x04u) {
            emit(MakeInternalMasterVolumeMessage(
                static_cast<uint16_t>(payload[0] & 0x7fu) * 129u));
            return true;
        }
        if (addressLow == 0x06u && payload[0] >= 40u &&
            payload[0] <= 88u) {
            emit(MakeInternalMasterTransposeMessage(payload[0]));
            return true;
        }
        if (addressLow == 0x00u && payloadSize >= 4u) {
            // XG MASTER TUNE is four nibbles forming 0x0000..0x07ff,
            // centered at 0x0400 and measured in tenths of a cent. Convert
            // once into the engine's higher-resolution centered 14-bit unit.
            const uint16_t raw = static_cast<uint16_t>(
                ((payload[0] & 0x0fu) << 12u) |
                ((payload[1] & 0x0fu) << 8u) |
                ((payload[2] & 0x0fu) << 4u) |
                (payload[3] & 0x0fu));
            if (raw <= 0x07ffu) {
                const int32_t delta = static_cast<int32_t>(raw) - 0x0400;
                const int32_t scaled = delta >= 0
                    ? (delta * 8192 + 500) / 1000
                    : (delta * 8192 - 500) / 1000;
                const int32_t centered = scaled + 8192;
                const uint16_t value14 = static_cast<uint16_t>(
                    centered < 0 ? 0 : (centered > 16383 ? 16383 : centered));
                emit(MakeInternalMasterFineTuneMessage(value14));
            }
            return true;
        }
        return true;
    }

    if (addressHigh != 0x08u || addressMiddle >= kChannelCount)
        return true;

    const uint8_t channel = addressMiddle;
    uint8_t parameter = addressLow;
    for (uint32_t index = 0u; index < payloadSize; ++index, ++parameter) {
        const uint8_t value = payload[index] & 0x7fu;
        switch (parameter) {
            case 0x01u:
                emit(MakeMidiControlChange(channel, 0u, value));
                break;
            case 0x02u:
                emit(MakeMidiControlChange(channel, 32u, value));
                break;
            case 0x03u:
                emit(MakeMidiProgramChange(channel, value));
                break;
            case 0x05u:
                emit(MakeMidiControlChange(channel,
                    value == 0u ? 126u : 127u, 0u));
                break;
            case 0x07u:
                emit(MakeInternalRhythmPartMessage(
                    channel, value == 0u ? 0u :
                        static_cast<uint8_t>(value > 2u ? 1u : value)));
                break;
            case 0x08u:
                if (value >= 40u && value <= 88u) {
                    emit(MakeMidiControlChange(channel, 101u, 0u));
                    emit(MakeMidiControlChange(channel, 100u, 2u));
                    emit(MakeMidiControlChange(channel, 6u, value));
                    emit(MakeMidiControlChange(channel, 101u, 127u));
                    emit(MakeMidiControlChange(channel, 100u, 127u));
                }
                break;
            case 0x0bu:
                emit(MakeMidiControlChange(channel, 7u, value));
                break;
            case 0x0eu:
                emit(MakeMidiControlChange(
                    channel, 10u, value == 0u ? 64u : value));
                break;
            default:
                break;
        }
    }
    return true;
}

template <typename Emit>
bool TranslateXGSystemExclusive(const uint8_t* data, uint32_t size,
                                Emit&& emit) {
    if (!data || size < 9u || data[0] != 0xf0u)
        return false;
    return TranslateXGSystemExclusivePayload(
        data + 1u, size - 1u, static_cast<Emit&&>(emit));
}

} // namespace svms

#endif
