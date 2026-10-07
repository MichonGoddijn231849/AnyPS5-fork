#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAWWRITERANGES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAWWRITERANGES_HPP

#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

// The guest ranges a draw may write, for the pipelined draws' ordering (APS5_PIPELINED_DRAWS: a later reader or
// writer of one waits for the draw's commit): the buffers its stages store to, the storage images they store to
// (the whole surface and its DCC keys), and its color, depth, stencil and metadata targets.
// `exact` (APS5_EXACT_DRAW_WRITES=1): the targets' ranges from their layouts (ColorTargetLayout's bytes per sample
// and slice, DepthSliceBytes per sample, one DCC key byte per 256 surface bytes, 4 HTILE bytes per 8x8 tile)
// instead of the estimates (twice the color bytes plus 64 KiB from the surface base, depth as 8 bytes per aligned
// pixel, metadata as a sixteenth plus 64 KiB). CMASK keeps its estimate in both.
std::vector<std::pair<std::uint64_t, std::uint64_t>> DrawWriteRanges(const Graphics::State& graphics, std::span<const Graphics::CompiledShader> stages, bool exact);
bool ExactDrawWrites();

}

#endif
