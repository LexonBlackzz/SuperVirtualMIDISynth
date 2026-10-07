#pragma once

#include "CanonicalEngineTypes.h"

#include <memory>
#include <filesystem>
#include <span>
#include <string>

namespace svms::canonical {

class CanonicalEngine {
public:
    CanonicalEngine(const std::filesystem::path& soundFontPath,
                    const CanonicalEngineConfig& config);
    ~CanonicalEngine();
    CanonicalEngine(CanonicalEngine&&) noexcept;
    CanonicalEngine& operator=(CanonicalEngine&&) noexcept;
    CanonicalEngine(const CanonicalEngine&) = delete;
    CanonicalEngine& operator=(const CanonicalEngine&) = delete;

    void reset();
    bool dispatch(const CanonicalEvent& event) noexcept;
    void renderBlock(std::uint64_t absoluteFrame,
                     std::span<float> outputLeft,
                     std::span<float> outputRight);
    void releaseAll(std::uint64_t absoluteFrame) noexcept;

    [[nodiscard]] CanonicalTelemetry telemetry() const noexcept;
    [[nodiscard]] const char* backendName() const noexcept;
    [[nodiscard]] static bool avx2Supported() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace svms::canonical
