#include "prx/libSceAgcDriver/Execution/include/DriverThreadClock.hpp"
#include <array>
#include <atomic>
#include <mutex>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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

}

void RegisterDriverThread(DriverThread role) noexcept {
    const auto index = static_cast<std::size_t>(role);
    if (index >= static_cast<std::size_t>(DriverThread::Count)) return;
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
    FILETIME creation{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(registered.threads[index], &creation, &exited, &kernel, &user)) return -1;
    const auto ticks = [](const FILETIME& time) { return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32u) | time.dwLowDateTime; };
    return static_cast<std::int64_t>((ticks(kernel) + ticks(user)) * 100u);
#else
    if (!registered.known[index]) return -1;
    timespec now{};
    if (clock_gettime(registered.clocks[index], &now) != 0) return -1;
    return static_cast<std::int64_t>(now.tv_sec) * 1000000000 + now.tv_nsec;
#endif
}

}
