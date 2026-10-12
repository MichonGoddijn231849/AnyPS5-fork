// The driver threads' CPU clocks (Execution/include/DriverThreadClock.hpp, read per frame by tools/FrameReplay.cpp,
// docs/dev/NIGHT_SHIFT.md B8b): a registered thread's clock counts its own CPU time, not the reader's, and an
// unregistered role reads -1.
#include "prx/libSceAgcDriver/Execution/include/DriverThreadClock.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

namespace {

bool check(bool condition, const char* what) {
    if (!condition) std::fprintf(stderr, "driver thread clock: %s\n", what);
    return condition;
}

// The calling thread's CPU time, read in that thread (the reference the driver's clock, read from another thread,
// is compared with).
std::int64_t OwnCpuNs() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
    const auto ticks = [](const FILETIME& time) { return (static_cast<std::int64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
    return (ticks(kernel) + ticks(user)) * 100;
#else
    timespec now{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
#endif
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
        // Spins until it has used 200 ms of CPU by its own clock (not wall time: a loaded machine may run it less
        // often), at most 10 s of wall time, then sleeps until told to exit.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        unsigned value = 1;
        while (OwnCpuNs() < 200'000'000 && std::chrono::steady_clock::now() < deadline) {
            for (int i = 0; i < 4096; ++i) value = value * 1664525u + 1013904223u;
        }
        sink = value;
        phase = 2;
        while (phase != 3) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    });
    while (phase == 0) std::this_thread::yield();
    while (phase != 2) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto spun = AgcDriver::DriverThreadCpuNs(DriverThread::Queue0Worker);
    ok &= check(spun >= 180'000'000, "the busy thread's clock counts its own spin (>= 180 ms of the 200 it measured)");
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
