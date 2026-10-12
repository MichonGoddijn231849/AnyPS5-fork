#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include <atomic>
#include <cstdio>
#include <string>
#include <stdexcept>

namespace AgcDriver::DriverDetail {

double TraceMs() {
    static const auto origin = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - origin).count();
}

void require(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(std::string("AGC driver: ") + reason);
    }
}

std::uint32_t readRegister(const Registers& registers, std::uint32_t offset) {
    const auto it = registers.find(offset);
    require(it != registers.end(), "required shader register has not been written");
    return it->second;
}

std::uint32_t readUserData(const Registers& shader, std::uint32_t offset) {
    const auto it = shader.find(offset);
    return it == shader.end() ? 0u : it->second;
}

std::string drawIoTag(const QueueState& queue, std::uint32_t queueId, std::uint64_t serial, std::uint32_t header) {
    const auto ps = std::uint64_t{readUserData(queue.shader, 0x008)} << 8u;
    const auto gs = std::uint64_t{readUserData(queue.shader, 0x088)} << 8u;
    const auto vs = gs != 0 ? gs : std::uint64_t{readUserData(queue.shader, 0x048)} << 8u;
    char text[160];
    std::snprintf(text, sizeof(text), "queue 0x%x submission %llu draw %llu %s ps 0x%llx vs 0x%llx", queueId, static_cast<unsigned long long>(serial), static_cast<unsigned long long>(DrawSerial()), Pm4::Name(header).c_str(), static_cast<unsigned long long>(ps), static_cast<unsigned long long>(vs));
    return text;
}

namespace {

thread_local std::uint64_t lastDrawSerial = 0;

}

std::uint64_t NextDrawSerial() {
    static std::atomic<std::uint64_t> serials{0};
    lastDrawSerial = serials.fetch_add(1, std::memory_order_relaxed);
    return lastDrawSerial;
}

std::uint64_t DrawSerial() {
    return lastDrawSerial;
}

}
