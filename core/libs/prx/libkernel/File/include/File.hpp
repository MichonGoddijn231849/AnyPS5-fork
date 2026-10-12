#ifndef CORE_LIBS_PRX_LIBKERNEL_FILE_FILE_HPP
#define CORE_LIBS_PRX_LIBKERNEL_FILE_FILE_HPP

#include <cstddef>
#include <cstdint>

#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

inline void TraceReadVolume(std::int64_t bytes) {
    static const bool enabled = std::getenv("APS5_TRACE_READS") != nullptr;
    if (!enabled || bytes < 0) return;
    static std::atomic<std::uint64_t> calls{0}, total{0};
    static std::atomic<std::int64_t> last{0};
    calls.fetch_add(1, std::memory_order_relaxed);
    total.fetch_add(static_cast<std::uint64_t>(bytes), std::memory_order_relaxed);
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto previous = last.load(std::memory_order_relaxed);
    if (now - previous < 10 || !last.compare_exchange_strong(previous, now)) return;
    std::fprintf(stderr, "[reads] (10 s) %llu calls, %.1f MiB\n", static_cast<unsigned long long>(calls.exchange(0)), static_cast<double>(total.exchange(0)) / 1048576.0);
}

extern "C" {

int APS5_VABI sceKernelOpen(const char* path, int flags, std::uint16_t mode);
int APS5_VABI sceKernelClose(int d);
std::int64_t APS5_VABI sceKernelRead(int d, void* buf, std::size_t nbytes);
std::int64_t APS5_VABI sceKernelWrite(int d, const void* buf, std::size_t nbytes);
std::int64_t APS5_VABI sceKernelLseek(int d, std::int64_t offset, int whence);
int APS5_VABI sceKernelStat(const char* path, FileStat* sb);
int APS5_VABI sceKernelUnlink(const char* path);
#ifdef _WIN32
std::int64_t NativePositioned_nid_no_patch(int descriptor, void* buf, std::size_t nbytes, std::int64_t offset, bool write);
#endif

}

#endif
