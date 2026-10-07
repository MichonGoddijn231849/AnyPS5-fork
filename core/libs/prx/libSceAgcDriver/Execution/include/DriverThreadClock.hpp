#pragma once

#include <cstdint>

namespace AgcDriver {

// The driver threads on its critical path, whose CPU time a measuring tool (tools/FrameReplay.cpp) reads per frame:
// queue 0's worker (decode, capture, recompile of the draws) and the pipelined draws' committer
// (APS5_PIPELINED_DRAWS, Graphics::Draw under the GPU lock).
enum class DriverThread : std::uint32_t {
    Queue0Worker,
    Committer,
    Count
};

// Registers the calling thread as `role` (the latest registration wins).
void RegisterDriverThread(DriverThread role) noexcept;
// The CPU time (user and kernel) the thread registered as `role` has consumed, in nanoseconds; -1 when none is
// registered or its clock cannot be read (the thread exited).
std::int64_t DriverThreadCpuNs(DriverThread role) noexcept;

}
