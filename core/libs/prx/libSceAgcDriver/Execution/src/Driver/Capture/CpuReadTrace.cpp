// Debug aid: APS5_TRACE_CPU_READS=<hex lo>-<hex hi> catches CPU accesses to guest memory in that range. Every
// APS5_TRACE_CPU_READS_EVERY flips (default 60) the registered pages of the range are made inaccessible; the
// first access to each page faults, is recorded (address, read or write, code address, thread) and the page gets
// its protection back, so execution goes on. The records of a round are printed at the next one, grouped by code
// address as module+offset: eboot.exe offsets are the game's own code. The GPU reads host imports regardless.
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <tuple>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace AgcDriver::DriverDetail {

extern bool (*CpuReadTraceProtectionHook)(std::uintptr_t address, unsigned long* protection);

#ifdef _WIN32
namespace {

struct Segment {
    std::uintptr_t begin;
    std::uintptr_t end;
    DWORD protection;
};

struct Record {
    std::uintptr_t address;
    std::uintptr_t rip;
    std::uint32_t thread;
    std::uint32_t write;
    // The first stack slots of the faulting thread: the caller of a C runtime copy is found among them.
    std::array<std::uintptr_t, 48> stack;
};

struct Trace {
    SRWLOCK lock = SRWLOCK_INIT;
    std::vector<Segment> armed;
    std::array<Record, 4096> records{};
    std::atomic<std::uint32_t> next{0};
    std::atomic<std::uint32_t> dropped{0};
};

Trace& State() {
    static auto* trace = new Trace();
    return *trace;
}

LONG CALLBACK OnFault(PEXCEPTION_POINTERS info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || info->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    const auto address = static_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionInformation[1]);
    auto& trace = State();
    AcquireSRWLockExclusive(&trace.lock);
    const auto it = std::upper_bound(trace.armed.begin(), trace.armed.end(), address, [](std::uintptr_t value, const Segment& segment) { return value < segment.begin; });
    if (it == trace.armed.begin() || address >= std::prev(it)->end) {
        ReleaseSRWLockExclusive(&trace.lock);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto& segment = *std::prev(it);
    const auto page = address & ~static_cast<std::uintptr_t>(4095);
    DWORD previous = 0;
    VirtualProtect(reinterpret_cast<void*>(page), 4096, segment.protection, &previous);
    ReleaseSRWLockExclusive(&trace.lock);
    const auto slot = trace.next.fetch_add(1, std::memory_order_relaxed);
    if (slot < trace.records.size()) {
        auto& record = trace.records[slot];
        record = {address, static_cast<std::uintptr_t>(info->ContextRecord->Rip), GetCurrentThreadId(), static_cast<std::uint32_t>(info->ExceptionRecord->ExceptionInformation[0]), {}};
        const auto* stack = reinterpret_cast<const std::uintptr_t*>(info->ContextRecord->Rsp);
        for (std::size_t i = 0; i < record.stack.size(); ++i) record.stack[i] = stack[i];
    } else {
        trace.dropped.fetch_add(1, std::memory_order_relaxed);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

}

bool CpuReadTraceProtection(std::uintptr_t address, unsigned long* protection) {
    auto& trace = State();
    AcquireSRWLockShared(&trace.lock);
    const auto it = std::upper_bound(trace.armed.begin(), trace.armed.end(), address, [](std::uintptr_t value, const Segment& segment) { return value < segment.begin; });
    const bool armed = it != trace.armed.begin() && address < std::prev(it)->end;
    if (armed) *protection = std::prev(it)->protection;
    ReleaseSRWLockShared(&trace.lock);
    return armed;
}

namespace {

std::string ModuleOffset(std::uintptr_t address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module) || module == nullptr) {
        char text[32];
        std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
        return text;
    }
    char path[MAX_PATH] = {};
    GetModuleFileNameA(module, path, MAX_PATH);
    std::string name(path);
    if (const auto slash = name.find_last_of("\\/"); slash != std::string::npos) name = name.substr(slash + 1);
    char text[64];
    std::snprintf(text, sizeof(text), "+0x%llx", static_cast<unsigned long long>(address - reinterpret_cast<std::uintptr_t>(module)));
    return name + text;
}

}

void Driver::cpuReadTraceTick() {
    static const std::pair<std::uint64_t, std::uint64_t> limit = [] {
        const char* text = std::getenv("APS5_TRACE_CPU_READS");
        if (text == nullptr) return std::pair<std::uint64_t, std::uint64_t>{0, 0};
        char* end = nullptr;
        const auto lo = std::strtoull(text, &end, 16);
        const auto hi = end != nullptr && *end == '-' ? std::strtoull(end + 1, nullptr, 16) : 0ull;
        return std::pair<std::uint64_t, std::uint64_t>{lo, hi};
    }();
    if (limit.second <= limit.first) return;
    static const std::uint64_t every = [] { const char* text = std::getenv("APS5_TRACE_CPU_READS_EVERY"); const auto value = text != nullptr ? std::strtoull(text, nullptr, 10) : 60ull; return value != 0 ? value : 60ull; }();
    static std::atomic<std::uint64_t> last{0};
    const auto flips = flipsCounted.load(std::memory_order_acquire);
    auto previous = last.load(std::memory_order_relaxed);
    if (flips < previous + every || !last.compare_exchange_strong(previous, flips)) return;
    static const bool installed = [] {
        CpuReadTraceProtectionHook = &CpuReadTraceProtection;
        return AddVectoredExceptionHandler(1, &OnFault) != nullptr;
    }();
    if (!installed) return;
    auto& trace = State();
    // Report the previous round, then arm the next.
    AcquireSRWLockExclusive(&trace.lock);
    for (const auto& segment : trace.armed) {
        DWORD old = 0;
        for (auto page = segment.begin; page < segment.end; page += 4096) VirtualProtect(reinterpret_cast<void*>(page), 4096, segment.protection, &old);
    }
    trace.armed.clear();
    const auto count = std::min<std::uint32_t>(trace.next.exchange(0), static_cast<std::uint32_t>(trace.records.size()));
    std::map<std::tuple<std::string, std::uint32_t>, std::pair<std::uint64_t, std::uintptr_t>> groups;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto& record = trace.records[i];
        // The code address, or for a fault inside a runtime library the first return address on the stack into the
        // game (eboot.exe) or a .prx module.
        std::string where = ModuleOffset(record.rip);
        if (where.rfind("eboot.exe", 0) != 0 && where.find(".prx") == std::string::npos) {
            for (const auto value : record.stack) {
                if (value < 0x10000) continue;
                MEMORY_BASIC_INFORMATION info{};
                if (VirtualQuery(reinterpret_cast<const void*>(value), &info, sizeof(info)) == 0 || info.Type != MEM_IMAGE || (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) == 0) continue;
                const auto caller = ModuleOffset(value);
                if (caller.rfind("eboot.exe", 0) == 0 || caller.find(".prx") != std::string::npos) {
                    where += " from " + caller;
                    break;
                }
            }
        }
        auto& group = groups[{where, record.write}];
        if (group.first++ == 0) group.second = record.address;
    }
    std::vector<std::pair<std::uint64_t, std::string>> lines;
    for (const auto& [key, value] : groups) {
        char text[320];
        std::snprintf(text, sizeof(text), "%s %s: %llu pages, first 0x%llx", std::get<0>(key).c_str(), std::get<1>(key) == 1 ? "write" : std::get<1>(key) == 8 ? "execute" : "read", static_cast<unsigned long long>(value.first), static_cast<unsigned long long>(value.second));
        lines.emplace_back(value.first, text);
    }
    std::sort(lines.begin(), lines.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    if (count != 0) std::fprintf(stderr, "[cpu-reads] flip %llu: %u first touches (%u dropped) by %zu code addresses:\n", static_cast<unsigned long long>(flips), count, trace.dropped.exchange(0), lines.size());
    for (std::size_t i = 0; i < lines.size() && i < 24; ++i) std::fprintf(stderr, "[cpu-reads]   %s\n", lines[i].second.c_str());
    // APS5_TRACE_CPU_READS_TRIGGER=<file>: arm only while that file exists (loading screens read files into
    // guest memory through the kernel, which fails on inaccessible pages instead of faulting).
    static const std::string trigger = [] { const char* text = std::getenv("APS5_TRACE_CPU_READS_TRIGGER"); return text != nullptr ? std::string(text) : std::string(); }();
    if (!trigger.empty() && GetFileAttributesA(trigger.c_str()) == INVALID_FILE_ATTRIBUTES) {
        ReleaseSRWLockExclusive(&trace.lock);
        return;
    }
    // Arm: every committed page of the registered ranges inside the limit, with its current protection.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    {
        const auto lease = GuestAllocations::GuestAllocationsAcquire_nid_postfix();
        for (const auto& range : lease) {
            const auto begin = std::max<std::uint64_t>(range->address, limit.first);
            const auto end = std::min<std::uint64_t>(range->address + range->bytes, limit.second);
            if (begin < end) ranges.emplace_back(begin & ~std::uint64_t{4095}, (end + 4095) & ~std::uint64_t{4095});
        }
    }
    for (const auto& [begin, end] : ranges) {
        for (auto cursor = static_cast<std::uintptr_t>(begin); cursor < end;) {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) == 0) break;
            const auto regionEnd = std::min<std::uintptr_t>(static_cast<std::uintptr_t>(end), reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize);
            const bool accessible = info.State == MEM_COMMIT && (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0 && (info.Protect & PAGE_GUARD) == 0;
            if (accessible) {
                DWORD old = 0;
                if (VirtualProtect(reinterpret_cast<void*>(cursor), regionEnd - cursor, PAGE_NOACCESS, &old)) trace.armed.push_back({cursor, regionEnd, info.Protect});
            }
            cursor = regionEnd;
        }
    }
    std::sort(trace.armed.begin(), trace.armed.end(), [](const Segment& a, const Segment& b) { return a.begin < b.begin; });
    ReleaseSRWLockExclusive(&trace.lock);
}
#else
void Driver::cpuReadTraceTick() {}
bool CpuReadTraceProtection(std::uintptr_t, unsigned long*) { return false; }
#endif

}
