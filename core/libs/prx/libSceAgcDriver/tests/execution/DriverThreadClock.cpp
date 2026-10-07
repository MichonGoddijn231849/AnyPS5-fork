// The driver threads' CPU clocks (Execution/include/DriverThreadClock.hpp, read per frame by tools/FrameReplay.cpp,
// docs/dev/NIGHT_SHIFT.md B8b): a registered thread's clock counts its own CPU time, not the reader's, and an
// unregistered role reads -1.
#include "prx/libSceAgcDriver/Execution/include/DriverThreadClock.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {

bool check(bool condition, const char* what) {
    if (!condition) std::fprintf(stderr, "driver thread clock: %s\n", what);
    return condition;
}

}

int main() {
    using AgcDriver::DriverThread;
    bool ok = check(AgcDriver::DriverThreadCpuNs(DriverThread::Committer) == -1, "an unregistered role reads -1");
    std::atomic<int> phase{0};
    std::atomic<unsigned> sink{0};
    std::thread busy([&] {
        AgcDriver::RegisterDriverThread(DriverThread::Queue0Worker);
        phase = 1;
        // Spins about 200 ms of CPU, then sleeps until told to exit.
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        unsigned value = 1;
        while (std::chrono::steady_clock::now() < until) value = value * 1664525u + 1013904223u;
        sink = value;
        phase = 2;
        while (phase != 3) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    });
    while (phase == 0) std::this_thread::yield();
    while (phase != 2) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto spun = AgcDriver::DriverThreadCpuNs(DriverThread::Queue0Worker);
    ok &= check(spun >= 100'000'000, "the busy thread's clock counts its own spin (>= 100 ms of 200)");
    // The reader's own sleep is not the registered thread's time.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto idle = AgcDriver::DriverThreadCpuNs(DriverThread::Queue0Worker);
    ok &= check(idle >= spun && idle - spun < 50'000'000, "a sleeping registered thread consumes almost no CPU");
    phase = 3;
    busy.join();
    std::printf("driver thread clock: spun %.1f ms, then %.1f ms while sleeping\n", static_cast<double>(spun) / 1e6, static_cast<double>(idle - spun) / 1e6);
    if (!ok) return 1;
    std::puts("driver thread clock tests passed");
    return 0;
}
