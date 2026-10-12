#include "DeepStack.hpp"
#include <cstddef>
#include <cstdint>
#include <exception>
#include <pthread.h>
#include <stdexcept>
#ifdef _WIN32
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0602
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <windows.h>
#endif

namespace ShaderRecompiler {

namespace {

constexpr std::size_t RequiredStack = std::size_t{2} << 20u;
constexpr std::size_t HelperStack = std::size_t{64} << 20u;

// Bytes left below the current frame, 0 when the frame is outside the bounds the thread reports (a stack switched
// without telling the OS). On Windows the TEB bounds follow libSceFiber's switches.
std::size_t StackLeft() {
    const volatile unsigned char marker = 0;
    const auto frame = reinterpret_cast<std::uintptr_t>(&marker);
    std::uintptr_t low = 0;
    std::uintptr_t high = 0;
#ifdef _WIN32
    ULONG_PTR lowLimit = 0;
    ULONG_PTR highLimit = 0;
    GetCurrentThreadStackLimits(&lowLimit, &highLimit);
    low = static_cast<std::uintptr_t>(lowLimit);
    high = static_cast<std::uintptr_t>(highLimit);
#else
    pthread_attr_t attributes;
    if (pthread_getattr_np(pthread_self(), &attributes) != 0) return 0;
    void* base = nullptr;
    std::size_t size = 0;
    if (pthread_attr_getstack(&attributes, &base, &size) == 0 && base != nullptr) {
        low = reinterpret_cast<std::uintptr_t>(base);
        high = low + size;
    }
    pthread_attr_destroy(&attributes);
#endif
    return frame > low && frame < high ? frame - low : 0;
}

struct Job {
    const std::function<void()>* work;
    std::exception_ptr failure;
};

void* RunJob(void* argument) {
    auto* job = static_cast<Job*>(argument);
    try {
        (*job->work)();
    } catch (...) {
        job->failure = std::current_exception();
    }
    return nullptr;
}

}

void RunWithDeepStack(const std::function<void()>& work) {
    if (StackLeft() >= RequiredStack) {
        work();
        return;
    }
    Job job{&work, nullptr};
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) throw std::runtime_error("RunWithDeepStack: pthread_attr_init failed");
    pthread_attr_setstacksize(&attributes, HelperStack);
    pthread_t helper{};
    const auto created = pthread_create(&helper, &attributes, RunJob, &job);
    pthread_attr_destroy(&attributes);
    if (created != 0) throw std::runtime_error("RunWithDeepStack: pthread_create failed");
    pthread_join(helper, nullptr);
    if (job.failure) std::rethrow_exception(job.failure);
}

}
