#pragma once

#include <cstdint>
#include <string>

namespace svms {

// 3D stage settings: the room, the listener and where each MIDI channel
// stands. Everything is applied at engine start; the stage is off by default
// and a disabled stage changes nothing in the audio path.
struct StageConfig {
    static constexpr uint32_t kMaxFurniture = 32u;

    struct Furniture {
        uint8_t kind = 0;        // FurnitureKind
        float x = 0.0f, y = 0.0f;
        bool rotated = false;
    };

    // What feeds the stage: 0 = the 16 MIDI channels, each seated on its own; 1 = the finished stereo
    // mix played through two loudspeakers in the room (no per-channel buses, so synthesis is unchanged).
    uint8_t mode = 0;
    // Speakers mode: the left and right speaker stand wherever they are put in the room and face the listener.
    bool speakerDirectivity = true;   // loudspeaker radiation pattern instead of an omnidirectional point
    float speaker[2][3] = {{10.75f, 26.17f, 1.2f}, {13.25f, 26.17f, 1.2f}};
    float speakerLevelDb = 0.0f;
    float speakerSend = 35.0f;        // share that rings in the room, like channelSend

    bool enabled = false;
    bool keepReverb = false;     // run the driver reverb after the stage too
    bool airAbsorption = true;
    bool earModel = true;
    uint32_t rays = 40000u;      // tail quality: 8000 .. 400000
    float tailTrim = 1.0f;       // tail level against the critical-distance balance

    float roomWidth = 24.0f, roomDepth = 36.0f, roomHeight = 14.0f;
    // Floor, ceiling, left, right, front, back (Material values).
    uint8_t surface[6] = {6, 0, 3, 3, 3, 7};

    float listener[3] = {12.0f, 24.0f, 1.2f};
    float listenerYaw = 0.0f;    // degrees clockwise from the +y direction

    float channel[16][3] = {
        {12.0f, 10.0f, 1.5f}, {12.0f, 10.0f, 1.5f}, {12.0f, 10.0f, 1.5f}, {12.0f, 10.0f, 1.5f},
        {12.0f, 10.0f, 1.5f}, {12.0f, 10.0f, 1.5f}, {12.0f, 10.0f, 1.5f}, {12.0f, 10.0f, 1.5f},
        {12.0f, 10.0f, 1.5f}, {12.0f, 10.0f, 1.5f}, {12.0f, 10.0f, 1.5f}, {12.0f, 10.0f, 1.5f},
        {12.0f, 10.0f, 1.5f}, {12.0f, 10.0f, 1.5f}, {12.0f, 10.0f, 1.5f}, {12.0f, 10.0f, 1.5f}};
    // Per channel: how wide it spreads on the plan (percent; drawn only), a level trim in dB on top
    // of the channel volume, and how much of it rings in the room (percent; 35 is the neutral send).
    float channelWidth[16] = {};
    float channelLevelDb[16] = {};
    float channelSend[16] = {35.0f, 35.0f, 35.0f, 35.0f, 35.0f, 35.0f, 35.0f, 35.0f,
                             35.0f, 35.0f, 35.0f, 35.0f, 35.0f, 35.0f, 35.0f, 35.0f};
    bool channelOn[16] = {true, true, true, true, true, true, true, true,
                          true, true, true, true, true, true, true, true};

    // An imported OBJ model replaces the box. Its size sets the room size the placement uses; the
    // materials are per group, 255 meaning the guess from the group's name.
    std::string meshPath;
    bool meshUpY = true;         // Y is up in the file (the stage is Z-up)
    float meshScale = 1.0f;
    uint8_t meshMaterial[24] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};

    uint32_t furnitureCount = 0u;
    Furniture furniture[kMaxFurniture];
};

} // namespace svms
