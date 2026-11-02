#pragma once

#include <cstdint>

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
    bool channelOn[16] = {true, true, true, true, true, true, true, true,
                          true, true, true, true, true, true, true, true};

    uint32_t furnitureCount = 0u;
    Furniture furniture[kMaxFurniture];
};

} // namespace svms
