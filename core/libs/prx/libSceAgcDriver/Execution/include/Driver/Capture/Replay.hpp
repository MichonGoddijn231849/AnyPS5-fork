#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_CAPTURE_REPLAY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_CAPTURE_REPLAY_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace AgcDriver::Capture {

std::uint64_t ReplayPacketsExecuted(std::uint32_t queue);
bool ReplayStalled();
bool ReplayDrain(std::chrono::milliseconds limit);
void ReplaySettle();
void ReplayRestoreQueueState(std::uint32_t queue, std::span<const std::byte> state);
void ReplayRestoreDriverState(bool resetGraphics, std::span<const std::byte> gds);
void ReplayRegisterShader(std::uint64_t codeAddress, std::uint64_t headerAddress, std::uint8_t type, std::span<const std::uint32_t> code, std::span<const std::byte> header);
void ReplayDumpNextPresent(std::string path, std::uint32_t scale);
void ReplayExpectCommands(std::uint64_t hash);
std::uint64_t ReplayCommandMismatches();

}

#endif
