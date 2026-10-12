#include "prx/libSceAgcDriver/Execution/include/DriverThreadClock.hpp"
#include <array>
#include <atomic>
#include <mutex>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#else
#include <pthread.h>
#include <time.h>
#endif

namespace AgcDriver {

namespace {

// Per role: the thread's CPU clock (Linux) or a real handle to it (Windows). Written at registration, read by
// another thread, so under a mutex; reads happen once per frame.
struct Registered {
    std::mutex mutex;
#ifdef _WIN32
    std::array<HANDLE, static_cast<std::size_t>(DriverThread::Count)> threads{};
#else
    std::array<clockid_t, static_cast<std::size_t>(DriverThread::Count)> clocks{};
    std::array<bool, static_cast<std::size_t>(DriverThread::Count)> known{};
#endif
};

Registered& Threads() {
    static auto* registered = new Registered();
    return *registered;
}

#ifdef _WIN32
// QueryThreadCycleTime counts time stamp counter cycles, which run at a constant rate: calibrated once against the
// performance counter over 20 ms (the first read pays it; only the replay tool reads the clocks).
double NsPerCycle() {
    static const double ratio = [] {
        LARGE_INTEGER frequency{}, start{}, stop{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        const auto cyclesStart = __rdtsc();
        Sleep(20);
        QueryPerformanceCounter(&stop);
        const auto cycles = __rdtsc() - cyclesStart;
        const double ns = static_cast<double>(stop.QuadPart - start.QuadPart) * 1e9 / static_cast<double>(frequency.QuadPart);
        return cycles != 0 ? ns / static_cast<double>(cycles) : 0.0;
    }();
    return ratio;
}
#endif

}

namespace {

thread_local int currentRole = -1;

}

int CurrentDriverThreadRole() noexcept {
    return currentRole;
}

void RegisterDriverThread(DriverThread role) noexcept {
    const auto index = static_cast<std::size_t>(role);
    if (index >= static_cast<std::size_t>(DriverThread::Count)) return;
    currentRole = static_cast<int>(index);
    auto& registered = Threads();
#ifdef _WIN32
    HANDLE thread = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread, THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) return;
    std::lock_guard lock(registered.mutex);
    if (registered.threads[index] != nullptr) CloseHandle(registered.threads[index]);
    registered.threads[index] = thread;
#else
    clockid_t clock{};
    if (pthread_getcpuclockid(pthread_self(), &clock) != 0) return;
    std::lock_guard lock(registered.mutex);
    registered.clocks[index] = clock;
    registered.known[index] = true;
#endif
}

std::int64_t DriverThreadCpuNs(DriverThread role) noexcept {
    const auto index = static_cast<std::size_t>(role);
    if (index >= static_cast<std::size_t>(DriverThread::Count)) return -1;
    auto& registered = Threads();
    std::lock_guard lock(registered.mutex);
#ifdef _WIN32
    if (registered.threads[index] == nullptr) return -1;
    // GetThreadTimes advances in scheduler ticks (15.6 ms), coarser than a frame; the thread's cycle count is exact.
    ULONG64 cycles = 0;
    if (!QueryThreadCycleTime(registered.threads[index], &cycles)) return -1;
    return static_cast<std::int64_t>(static_cast<double>(cycles) * NsPerCycle());
#else
    if (!registered.known[index]) return -1;
    timespec now{};
    if (clock_gettime(registered.clocks[index], &now) != 0) return -1;
    return static_cast<std::int64_t>(now.tv_sec) * 1000000000 + now.tv_nsec;
#endif
}

namespace {

std::atomic<std::uint64_t> drawPackets{0};
std::atomic<std::uint64_t> drawsCommitted{0};

}

std::uint64_t DriverDrawPackets() noexcept {
    return drawPackets.load(std::memory_order_relaxed);
}

std::uint64_t DriverDrawsCommitted() noexcept {
    return drawsCommitted.load(std::memory_order_relaxed);
}

void CountDriverDrawPacket() noexcept {
    drawPackets.fetch_add(1, std::memory_order_relaxed);
}

void CountDriverDrawCommitted() noexcept {
    drawsCommitted.fetch_add(1, std::memory_order_relaxed);
}

}
