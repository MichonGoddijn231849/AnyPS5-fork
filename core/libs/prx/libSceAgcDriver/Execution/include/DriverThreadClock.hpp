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
// The role the calling thread registered as, -1 for none: a measuring tool's allocation counter attributes by it.
int CurrentDriverThreadRole() noexcept;
// The CPU time (user and kernel) the thread registered as `role` has consumed, in nanoseconds; -1 when none is
// registered or its clock cannot be read (the thread exited).
std::int64_t DriverThreadCpuNs(DriverThread role) noexcept;

// Draw packets that reached the worker's draw tail, and draws committed (Graphics::Draw calls through commitDraw),
// over the process: a measuring tool divides the threads' CPU time by them.
std::uint64_t DriverDrawPackets() noexcept;
std::uint64_t DriverDrawsCommitted() noexcept;
void CountDriverDrawPacket() noexcept;
void CountDriverDrawCommitted() noexcept;
// Recorded batches submitted (Graphics::Recorder::Submit of an open batch), over the process: the tool's batches per
// flip (docs/dev/NIGHT_SHIFT.md B2). Defined with the recorder (Graphics/src/Recorder.cpp), which the draw tests link
// without this file.
std::uint64_t DriverBatchesSubmitted() noexcept;
void CountDriverBatchSubmitted() noexcept;

}
