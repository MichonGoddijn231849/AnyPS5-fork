#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SUBMISSION_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SUBMISSION_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace AgcDriver::DriverDetail {

struct Submission {
    std::uint64_t serial;
    std::uint32_t queue;
    std::vector<std::uint32_t> commands;

    std::shared_ptr<const ShaderRegistry> shaders;
    std::map<std::size_t, std::shared_ptr<IFlipRequest>> flips;
    std::map<std::size_t, std::shared_ptr<IRenderingWait>> renderingWaits;
    bool suspend = false;
    bool waitFree = false;

    std::uint64_t received = 0;
    std::vector<std::uint64_t> labelWrites;
    std::set<std::size_t> heldAtSubmit;
    std::chrono::steady_clock::time_point enqueuedAt{};
    const std::uint32_t* rewindTail = nullptr;
    std::size_t rewindWords = 0;
    // The guest command buffer the commands were copied from (a frame capture carries the rest of a
    // submission blocked in a wait from there).
    const std::uint32_t* source = nullptr;
};

// A submission a frame capture carries unfinished: where the rest of its commands start in guest memory.
struct BlockedWait {
    const std::uint32_t* source = nullptr;
    std::size_t cursor = 0;
    std::size_t words = 0;
    // The driver's copy of the commands (guest memory may already hold other commands).
    const std::uint32_t* commands = nullptr;
    std::uint64_t received = 0;
    std::uint32_t queue = 0;
};

struct QueueWorker {
    std::deque<Submission> pending;

    std::atomic<std::uint64_t> queued{0};
    std::unordered_map<std::uint64_t, std::uint32_t> unfinishedWrites;
    std::thread thread;
};

}

#endif
