#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_CAPTURE_REPLAY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_CAPTURE_REPLAY_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <shared_mutex>
#include <utility>

namespace AgcDriver::Capture {

enum CacheClass : std::uint32_t {
    CacheDispatch = 1,
    CacheDraw = 2,
    CacheResources = 4,
    CacheTextures = 8,
    CacheTables = 16,
    CacheSpace = 32,
    CacheAll = 63,
    CacheLive = CacheDispatch | CacheDraw | CacheResources | CacheTables | CacheSpace,
};

std::uint64_t ReplayPacketsExecuted(std::uint32_t queue);
void ReplayClearCaches(std::uint32_t classes);
bool ReplayStalled();
bool ReplayDrain(std::chrono::milliseconds limit);
void ReplaySettle();
// Held exclusively while the replay writes a memory delta. A released REWIND copies the commands the CPU
// appended under it shared, so it never reads a delta half written (the replay writes pages in parallel,
// the control word's page can land before the commands').
std::shared_mutex& ReplayMemoryWriteMutex();
// Collects the CPU writes over the ranges (a restore's), stamping them as any collect would: the
// write-watch walk and reset then happen here instead of inside the next frame's first lookups.
void ReplayCollectWrites(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges);
void ReplayRestoreQueueState(std::uint32_t queue, std::span<const std::byte> state);
void ReplayRestoreDriverState(bool resetGraphics, std::span<const std::byte> gds);
void ReplayRegisterShader(std::uint64_t codeAddress, std::uint64_t headerAddress, std::uint8_t type, std::span<const std::uint32_t> code, std::span<const std::byte> header);
void ReplayDumpNextPresent(std::string path, std::uint32_t scale);
void ReplayExpectCommands(std::uint64_t hash);
std::uint64_t ReplayCommandMismatches();

}

#endif
