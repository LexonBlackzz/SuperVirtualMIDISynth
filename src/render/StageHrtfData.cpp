// The generic head table (render/data/hrtf_generic.svhr) as bytes.

#include "render/StageHrtf.h"

#include "StageHead.inc"

namespace svms {
namespace stage {

const unsigned char* EmbeddedHeadTable(size_t& size) noexcept {
    size = sizeof(kStageHeadBytes);
    return kStageHeadBytes;
}

} // namespace stage
} // namespace svms
