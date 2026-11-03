#pragma once

// A StageConfig as short text parts, for the runtime-link command area (348
// bytes per command): part 0 is the room and listener, parts 1-2 hold eight
// channels each, parts 3 and up hold eight pieces of furniture each. The
// receiver collects the parts and applies them on a commit.

#include "synth/StageConfig.h"

#include <cstdint>
#include <cstdio>

namespace svms {

inline constexpr uint32_t kStageWireChannelParts = 2u;
inline constexpr uint32_t kStageWireFurnitureBase = 3u;
inline constexpr uint32_t kStageWireCommit = 0x100u;

inline uint32_t StageWirePartCount(const StageConfig& s) {
    return kStageWireFurnitureBase + (s.furnitureCount + 7u) / 8u;
}

inline bool EncodeStagePart(const StageConfig& s, uint32_t part, char* out, size_t cap) {
    int n = 0;
    auto put = [&](const char* fmt, auto... a) {
        if (n >= 0 && static_cast<size_t>(n) < cap) n += std::snprintf(out + n, cap - static_cast<size_t>(n), fmt, a...);
    };
    if (part == 0u) {
        put("%d;%d;%d;%d;%u;%.4f;%.2f;%.2f;%.2f;", s.enabled ? 1 : 0, s.keepReverb ? 1 : 0,
            s.airAbsorption ? 1 : 0, s.earModel ? 1 : 0, s.rays, s.tailTrim, s.roomWidth, s.roomDepth,
            s.roomHeight);
        put("%d,%d,%d,%d,%d,%d;%.3f,%.3f,%.3f;%.2f;%u", s.surface[0], s.surface[1], s.surface[2],
            s.surface[3], s.surface[4], s.surface[5], s.listener[0], s.listener[1], s.listener[2],
            s.listenerYaw, s.furnitureCount);
    } else if (part <= kStageWireChannelParts) {
        for (uint32_t i = (part - 1u) * 8u; i < part * 8u; ++i)
            put("%.3f,%.3f,%.3f,%d,%.0f,%.1f,%.0f;", s.channel[i][0], s.channel[i][1], s.channel[i][2],
                s.channelOn[i] ? 1 : 0, s.channelWidth[i], s.channelLevelDb[i], s.channelSend[i]);
    } else {
        const uint32_t first = (part - kStageWireFurnitureBase) * 8u;
        for (uint32_t i = first; i < first + 8u && i < s.furnitureCount; ++i)
            put("%d,%.2f,%.2f,%d;", s.furniture[i].kind, s.furniture[i].x, s.furniture[i].y,
                s.furniture[i].rotated ? 1 : 0);
    }
    return n > 0 && static_cast<size_t>(n) < cap;
}

inline bool DecodeStagePart(StageConfig& s, uint32_t part, const char* text) {
    if (!text) return false;
    if (part == 0u) {
        int en, keep, air, ear, sf[6], count;
        unsigned rays;
        float trim, w, d, h, l[3], yaw;
        int used = 0;
        if (std::sscanf(text, "%d;%d;%d;%d;%u;%f;%f;%f;%f;%n", &en, &keep, &air, &ear, &rays, &trim, &w, &d, &h,
                        &used) != 9)
            return false;
        if (std::sscanf(text + used, "%d,%d,%d,%d,%d,%d;%f,%f,%f;%f;%d", &sf[0], &sf[1], &sf[2], &sf[3], &sf[4],
                        &sf[5], &l[0], &l[1], &l[2], &yaw, &count) != 11)
            return false;
        s.enabled = en != 0;
        s.keepReverb = keep != 0;
        s.airAbsorption = air != 0;
        s.earModel = ear != 0;
        s.rays = rays < 8000u ? 8000u : rays > 400000u ? 400000u : rays;
        s.tailTrim = trim < 0.0f ? 0.0f : trim > 4.0f ? 4.0f : trim;
        s.roomWidth = w;
        s.roomDepth = d;
        s.roomHeight = h;
        for (int i = 0; i < 6; ++i) s.surface[i] = static_cast<uint8_t>(sf[i]);
        for (int i = 0; i < 3; ++i) s.listener[i] = l[i];
        s.listenerYaw = yaw;
        s.furnitureCount = count < 0 ? 0u : count > static_cast<int>(StageConfig::kMaxFurniture)
                                                ? StageConfig::kMaxFurniture
                                                : static_cast<uint32_t>(count);
        return true;
    }
    const char* p = text;
    if (part <= kStageWireChannelParts) {
        for (uint32_t i = (part - 1u) * 8u; i < part * 8u; ++i) {
            float x, y, z, wd, db, sd;
            int on, used = 0;
            if (std::sscanf(p, "%f,%f,%f,%d,%f,%f,%f;%n", &x, &y, &z, &on, &wd, &db, &sd, &used) != 7) return false;
            s.channel[i][0] = x;
            s.channel[i][1] = y;
            s.channel[i][2] = z;
            s.channelOn[i] = on != 0;
            s.channelWidth[i] = wd < 0.0f ? 0.0f : wd > 100.0f ? 100.0f : wd;
            s.channelLevelDb[i] = db < -24.0f ? -24.0f : db > 12.0f ? 12.0f : db;
            s.channelSend[i] = sd < 0.0f ? 0.0f : sd > 100.0f ? 100.0f : sd;
            p += used;
        }
        return true;
    }
    const uint32_t first = (part - kStageWireFurnitureBase) * 8u;
    for (uint32_t i = first; i < first + 8u && i < s.furnitureCount && i < StageConfig::kMaxFurniture; ++i) {
        int kind, rot, used = 0;
        float x, y;
        if (std::sscanf(p, "%d,%f,%f,%d;%n", &kind, &x, &y, &rot, &used) != 4) return false;
        s.furniture[i].kind = static_cast<uint8_t>(kind);
        s.furniture[i].x = x;
        s.furniture[i].y = y;
        s.furniture[i].rotated = rot != 0;
        p += used;
    }
    return true;
}

} // namespace svms
