#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DIAGNOSTICS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DIAGNOSTICS_HPP

#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <cstdint>
#include <string>

namespace AgcDriver::DriverDetail {

double TraceMs();

void require(bool condition, const char* reason);

std::uint32_t readRegister(const Registers& registers, std::uint32_t offset);

std::uint32_t readUserData(const Registers& shader, std::uint32_t offset);

// The head of a draw's [draw-io] line (APS5_TRACE_DRAW_IO, see Graphics::PassTag): queue, submission, packet name,
// and the pixel and first geometry program addresses from the registers.
std::string drawIoTag(const QueueState& queue, std::uint32_t queueId, std::uint64_t serial, std::uint32_t header);

// Every draw packet's serial (counted from 0 over the process, at Driver::draw), named in [draw-io] lines and by
// APS5_CAPTURE_PASSES; DrawSerial() is the one the thread's last draw packet got.
std::uint64_t NextDrawSerial();
std::uint64_t DrawSerial();

}

#endif
