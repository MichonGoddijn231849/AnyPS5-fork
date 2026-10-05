#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <vector>
#include <stdexcept>

namespace AgcDriver::DriverDetail {

namespace {

// Debug aid (APS5_CHECK_BVH=1): before each dispatch of the BVH refit program (GTA V's RT, found by its first
// code words), and before every later dispatch that gets a blob the refit used in user data 4-5, that
// PSR_BVHL blob is checked the way the refit walks it: every leaf child must start a list inside the list
// array that ends at an entry with its end bit (second dword negative) or type 7. When a blob has more bad
// leaves than at its last check, the bad nodes and the dispatches since that check are printed, to find
// what first corrupts it.
struct BvhCheck {
    std::mutex mutex;
    std::map<std::uint64_t, std::size_t> badLeaves;
    std::map<std::uint64_t, std::uint64_t> checkedAt;
    std::map<std::uint64_t, std::vector<std::byte>> lastBytes;
    std::deque<std::pair<std::uint64_t, std::string>> recent;
    std::uint64_t sequence = 0;
    std::uint64_t checks = 0;
    std::uint64_t badChecks = 0;
    double reportedAt = -1e9;
};

BvhCheck& Bvh() {
    static BvhCheck check;
    return check;
}

void NoteDispatchForBvh(std::uint32_t queue, std::uint64_t serial, std::uint64_t program, std::span<const std::uint32_t> userData) {
    std::string text = std::to_string(TraceMs()) + " ms queue 0x" + [&] { char q[16]; std::snprintf(q, sizeof(q), "%x", queue); return std::string(q); }() + " submission " + std::to_string(serial) + " program 0x";
    char item[24];
    std::snprintf(item, sizeof(item), "%llx", static_cast<unsigned long long>(program));
    text += item;
    for (const auto word : userData) {
        std::snprintf(item, sizeof(item), " %08x", word);
        text += item;
    }
    auto& check = Bvh();
    std::lock_guard lock(check.mutex);
    check.recent.emplace_back(++check.sequence, std::move(text));
    if (check.recent.size() > 4096) check.recent.pop_front();
}

std::atomic<std::uint64_t> checkAfterRefit{0};

bool KnownBvh(std::uint64_t pointer) {
    auto& check = Bvh();
    std::lock_guard lock(check.mutex);
    return check.badLeaves.contains(pointer);
}

void CheckBvh(std::uint64_t pointer) {
    const auto* blob = reinterpret_cast<const std::byte*>(static_cast<std::uintptr_t>(pointer));
    const auto dword = [&](std::uint64_t offset) {
        std::uint32_t value;
        std::memcpy(&value, blob + offset, sizeof(value));
        return value;
    };
    GuestMemory::FlushGpuWrites(pointer, 0x80);
    if (std::memcmp(blob, "PSR_BVHL", 8) != 0) return;
    const auto bytes = dword(0x10) | (static_cast<std::uint64_t>(dword(0x14)) << 32u);
    GuestMemory::FlushGpuWrites(pointer, static_cast<std::size_t>(bytes));
    const auto aOffset = dword(0x40) | (static_cast<std::uint64_t>(dword(0x44)) << 32u);
    const auto nodes = dword(0x50);
    const auto entries = dword(0x58);
    const auto threads = dword(0x60);
    Graphics::DebugWatchRange(pointer, pointer + bytes);
    if (aOffset + 4ull * ((threads + 3) / 4) > bytes || 64ull * nodes > bytes || 8ull * entries > bytes) return;
    std::size_t bad = 0;
    std::string first;
    for (std::uint32_t t = 0; t < threads; ++t) {
        const auto node = dword(aOffset + 4ull * (t >> 2u));
        if (node >= nodes) continue;
        const auto child = dword(64ull * node + 4ull * (t & 3u));
        if (child == 30u || child == 0xffffffffu || (child & 7u) != 0) continue;
        bool ended = false;
        for (auto index = child >> 3u; index < entries && index - (child >> 3u) < 64u; ++index) {
            if ((dword(8ull * index) & 7u) == 7u || (dword(8ull * index + 4) >> 31u) != 0) {
                ended = true;
                break;
            }
        }
        if (ended) continue;
        if (bad++ < 4) {
            char item[96];
            std::snprintf(item, sizeof(item), " node %u child 0x%08x;", node, child);
            first += item;
        }
    }
    auto& check = Bvh();
    std::lock_guard lock(check.mutex);
    // APS5_CHECK_BVH_TRIGGER=<file>: the first check creates that file (APS5_CAPTURE_TRIGGER's), so a frame
    // capture starts with the first frames a refit runs in.
    if (check.badLeaves.empty()) {
        if (const char* trigger = std::getenv("APS5_CHECK_BVH_TRIGGER"); trigger != nullptr && *trigger != '\0') {
            if (FILE* file = std::fopen(trigger, "wb")) std::fclose(file);
            std::fprintf(stderr, "[bvh] first check: created %s\n", trigger);
        }
    }
    ++check.checks;
    if (bad != 0) ++check.badChecks;
    if (const auto now = TraceMs(); now - check.reportedAt >= 10000.0) {
        std::fprintf(stderr, "[bvh] %.0f ms: %llu checks of %zu blobs so far, %llu with bad leaves\n", now, static_cast<unsigned long long>(check.checks), check.badLeaves.size() + (check.badLeaves.contains(pointer) ? 0 : 1), static_cast<unsigned long long>(check.badChecks));
        check.reportedAt = now;
    }
    auto& known = check.badLeaves[pointer];
    auto& checkedAt = check.checkedAt[pointer];
    const auto since = checkedAt;
    // The dispatch being checked was noted already: the next check covers it.
    checkedAt = check.sequence - 1;
    if (bad <= known) {
        check.lastBytes[pointer].assign(blob, blob + bytes);
        return;
    }
    std::fprintf(stderr, "[bvh] %.1f ms blob 0x%llx: %zu bad leaves (was %zu):%s dispatches since its last check (the last one is the next to run), oldest first:\n", TraceMs(), static_cast<unsigned long long>(pointer), bad, known, first.c_str());
    const auto newer = static_cast<std::size_t>(std::count_if(check.recent.begin(), check.recent.end(), [&](const auto& entry) { return entry.first > since; }));
    std::size_t skipped = newer > 600 ? newer - 600 : 0;
    if (skipped != 0) std::fprintf(stderr, "[bvh]   (%zu older ones left out)\n", skipped);
    for (const auto& [sequence, text] : check.recent) {
        if (sequence <= since || (skipped != 0 && skipped-- != 0)) continue;
        std::fprintf(stderr, "[bvh]   %s\n", text.c_str());
    }
    known = bad;
    // The blob as at its last check and now, for the first few reports (bvh_<blob>_<n>_{before,after}.bin).
    static int dumps = 0;
    if (dumps < 4) {
        char name[96];
        std::snprintf(name, sizeof(name), "bvh_%llx_%d_before.bin", static_cast<unsigned long long>(pointer), dumps);
        if (FILE* file = std::fopen(name, "wb")) {
            const auto& before = check.lastBytes[pointer];
            std::fwrite(before.data(), 1, before.size(), file);
            std::fclose(file);
        }
        std::snprintf(name, sizeof(name), "bvh_%llx_%d_after.bin", static_cast<unsigned long long>(pointer), dumps);
        if (FILE* file = std::fopen(name, "wb")) {
            std::fwrite(blob, 1, static_cast<std::size_t>(bytes), file);
            std::fclose(file);
        }
        std::fprintf(stderr, "[bvh]   blob written to %s and the matching _before.bin\n", name);
        ++dumps;
    }
    check.lastBytes[pointer].assign(blob, blob + bytes);
}

}

void Driver::dispatch(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission, std::uint64_t indirectArguments) {
    const auto address = (static_cast<std::uint64_t>(readRegister(queue.shader, 0x20c)) << 8u) | (static_cast<std::uint64_t>(readRegister(queue.shader, 0x20d) & 0xffu) << 40u);
    auto it = submission.shaders->upper_bound(address);
    require(it != submission.shaders->begin(), "compute program does not belong to a registered shader");
    --it;
    // Debug aid: APS5_SKIP_PROGRAMS=<hex[:n],...> drops the dispatches of those compute programs (with :n
    // only the program's n-th dispatch, counted from 0 over the process), to tell whether a GPU fault or
    // hang comes from them.
    struct Skip {
        std::uint64_t program;
        std::uint64_t nth;
    };
    static constexpr std::uint64_t EveryDispatch = ~std::uint64_t{0};
    static const std::vector<Skip> skipped = [] {
        std::vector<Skip> parsed;
        const char* text = std::getenv("APS5_SKIP_PROGRAMS");
        while (text != nullptr && *text != 0) {
            char* end = nullptr;
            const auto value = std::strtoull(text, &end, 16);
            if (end == text) break;
            auto nth = EveryDispatch;
            if (*end == ':') nth = std::strtoull(end + 1, &end, 10);
            parsed.push_back({value, nth});
            text = *end == ',' ? end + 1 : end;
        }
        return parsed;
    }();
    if (!skipped.empty()) {
        static std::mutex countsMutex;
        static std::map<std::uint64_t, std::uint64_t> counts;
        std::uint64_t nth = 0;
        {
            std::lock_guard lock(countsMutex);
            nth = counts[address]++;
        }
        if (std::any_of(skipped.begin(), skipped.end(), [&](const Skip& skip) { return skip.program == address && (skip.nth == EveryDispatch || skip.nth == nth); })) return;
    }
    const auto& snapshot = *it->second;
    require(address - snapshot.codeAddress < snapshot.code.size() * sizeof(std::uint32_t), "compute program is outside registered shader code");
    require(snapshot.type == 0, "compute program refers to a non-compute shader");
    const auto userCount = (readRegister(queue.shader, 0x213) >> 1u) & 0x1fu;
    std::vector<std::uint32_t> userData;
    for (std::uint32_t i = 0; i < userCount; ++i) {
        userData.push_back(readRegister(queue.shader, 0x240 + i));
    }
    // Debug aid: APS5_WATCH_PROGRAM=<hex> prints that compute program's dispatch size and user data, and
    // the 16 qwords behind the pointer in user data 4-5 (guest memory as it is, GPU writes may be pending);
    // APS5_WATCH_PROGRAM=1 prints every dispatch's program, size and user data.
    static const std::uint64_t watchedProgram = [] {
        const char* text = std::getenv("APS5_WATCH_PROGRAM");
        return text != nullptr ? std::strtoull(text, nullptr, 16) : 0ull;
    }();
    if (address == watchedProgram || watchedProgram == 1) {
        std::string text;
        char item[48];
        for (const auto word : userData) {
            std::snprintf(item, sizeof(item), " %08x", word);
            text += item;
        }
        std::fprintf(stderr, "[watch] program 0x%llx dispatch %u x %u x %u, user data%s\n", static_cast<unsigned long long>(address), packet.size() > 1 ? packet[1] : 0, packet.size() > 2 ? packet[2] : 0, packet.size() > 3 ? packet[3] : 0, text.c_str());
        if (address == watchedProgram && userData.size() >= 6) {
            const auto pointer = (userData[4] | (static_cast<std::uint64_t>(userData[5]) << 32u)) & 0xffffffffffffull;
            text.clear();
            for (std::size_t i = 0; i < 16; ++i) {
                std::snprintf(item, sizeof(item), " %016llx", static_cast<unsigned long long>(reinterpret_cast<const volatile std::uint64_t*>(pointer)[i]));
                text += item;
            }
            std::fprintf(stderr, "[watch]   0x%llx:%s\n", static_cast<unsigned long long>(pointer), text.c_str());
        }
    }
    static const bool checkBvh = std::getenv("APS5_CHECK_BVH") != nullptr;
    if (checkBvh) {
        // The blob a refit just used is checked again at the next dispatch, after the refit ran.
        if (const auto refitted = checkAfterRefit.exchange(0, std::memory_order_acq_rel); refitted != 0) CheckBvh(refitted);
        NoteDispatchForBvh(submission.queue, submission.serial, address, userData);
        static constexpr std::array<std::uint32_t, 6> RefitStart{0xbfa00003u, 0xd7460001u, 0x04010c06u, 0xf4041a82u, 0xfa000040u, 0xbf8cc07fu};
        const auto word = (address - snapshot.codeAddress) / sizeof(std::uint32_t);
        const auto pointer = userData.size() >= 6 ? (userData[4] | (static_cast<std::uint64_t>(userData[5]) << 32u)) & 0xffffffffffffull : 0;
        const bool refit = word + RefitStart.size() <= snapshot.code.size() && std::equal(RefitStart.begin(), RefitStart.end(), snapshot.code.begin() + static_cast<std::ptrdiff_t>(word));
        if (pointer != 0 && (refit || KnownBvh(pointer))) CheckBvh(pointer);
        if (refit && pointer != 0) checkAfterRefit.store(pointer, std::memory_order_release);
        // The pass before a frame's BVH updates (its blob in user data 0-1) shows the blob as built.
        static constexpr std::array<std::uint32_t, 6> SetupStart{0xbfa00003u, 0xd7460001u, 0x04010c0bu, 0xf4041a80u, 0xfa000048u, 0xbf8cc07fu};
        if (userData.size() >= 2 && word + SetupStart.size() <= snapshot.code.size() && std::equal(SetupStart.begin(), SetupStart.end(), snapshot.code.begin() + static_cast<std::ptrdiff_t>(word))) {
            CheckBvh((userData[0] | (static_cast<std::uint64_t>(userData[1]) << 32u)) & 0xffffffffffffull);
        }
        // APS5_CHECK_BVH_BUILD_TRIGGER=<file>: the first dispatch of the BVH build kernel creates that file
        // (APS5_CAPTURE_TRIGGER's), so a frame capture holds the builds of the following frames.
        static constexpr std::array<std::uint32_t, 6> BuildStart{0xbfa00003u, 0xd7460003u, 0x04010c0fu, 0x7d06060eu, 0xbeea086au, 0xbefe046au};
        static std::atomic<bool> buildTriggered{false};
        if (userData.size() >= 2 && word + BuildStart.size() <= snapshot.code.size() && std::equal(BuildStart.begin(), BuildStart.end(), snapshot.code.begin() + static_cast<std::ptrdiff_t>(word))) {
            const auto blob = (userData[0] | (static_cast<std::uint64_t>(userData[1]) << 32u)) & 0xffffffffffffull;
            GuestMemory::FlushGpuWrites(blob, 0x80);
            const auto* header = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(blob));
            std::uint64_t bytes = 0;
            if (std::memcmp(header, "PSR_BVHL", 8) == 0) std::memcpy(&bytes, header + 0x10, sizeof(bytes));
            std::fprintf(stderr, "[bvh] %.1f ms queue 0x%x: build of blob 0x%llx (%llu bytes)\n", TraceMs(), submission.queue, static_cast<unsigned long long>(blob), static_cast<unsigned long long>(bytes));
            if (bytes != 0 && bytes < (1ull << 30u)) Graphics::DebugWatchRange(blob, blob + bytes);
        }
        if (word + BuildStart.size() <= snapshot.code.size() && std::equal(BuildStart.begin(), BuildStart.end(), snapshot.code.begin() + static_cast<std::ptrdiff_t>(word)) && !buildTriggered.exchange(true)) {
            if (const char* trigger = std::getenv("APS5_CHECK_BVH_BUILD_TRIGGER"); trigger != nullptr && *trigger != '\0') {
                if (FILE* file = std::fopen(trigger, "wb")) std::fclose(file);
                std::fprintf(stderr, "[bvh] %.1f ms: first BVH build dispatch, created %s\n", TraceMs(), trigger);
            }
        }
    }
    auto compute = Graphics::DecodeComputeStageInfo(queue.shader, snapshot.header);
    const std::array<ShaderRecompiler::MemoryRegion, 2> memory{{{snapshot.codeAddress, std::as_bytes(std::span(snapshot.code))}, {snapshot.headerAddress, snapshot.header}}};

    static const bool unlockedDevice = std::getenv("APS5_NO_UNLOCKED_DEVICE") == nullptr;
    std::shared_ptr<VulkanDevice> localDevice = unlockedDevice ? device.Load() : nullptr;
    if (localDevice == nullptr) {
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Dispatch);
        std::lock_guard gpuLock(GuestMemory::GpuMutex());
        if (device == nullptr) device = std::make_shared<VulkanDevice>();
        localDevice = device;
    }
    const auto codeOffset = static_cast<std::size_t>((address - snapshot.codeAddress) / sizeof(std::uint32_t));
    std::array<std::uint32_t, 5> resolved{};
    if (indirectArguments != 0 && matchesFillKernel(std::span(snapshot.code).subspan(codeOffset), userData, compute)) {

        recordQueuedLabelsBeforeRead(submission.queue);
        const auto readStart = std::chrono::steady_clock::now();
        resolved = Pm4::ReadDispatchArguments(indirectArguments, packet[4]);
        countIndirect(IndirectFillKernel, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readStart).count());
        packet = resolved;
        indirectArguments = 0;
    }
    if (fillBuffer(queue, submission.queue, packet, std::span(snapshot.code).subspan(codeOffset), userData, compute, localDevice)) {
        pendingDispatchPhases().outcome = DispatchOutcome::FillHle;
        return;
    }
    if (indirectArguments == 0 && copyBuffer(queue, submission.queue, packet, std::span(snapshot.code).subspan(codeOffset), userData, compute, localDevice, address)) {
        pendingDispatchPhases().outcome = DispatchOutcome::CopyHle;
        return;
    }
    if (indirectArguments == 0 && (packet[4] & 0x20u) != 0) {
        const std::array<std::uint32_t, 3> threads{packet[1], packet[2], packet[3]};
        for (std::uint32_t axis = 0; axis < 3; ++axis) {
            if (threads[axis] % compute.numThreads[axis] != 0) compute.partialThreads = threads;
        }
    }
    ShaderRecompiler::RecompileRequest request{
        {ShaderRecompiler::ShaderStage::Compute, address, std::span(snapshot.code).subspan(codeOffset), snapshot.headerAddress, snapshot.header},
        {(packet[4] & 0x8000u) != 0 ? 32u : 64u, 0, userData, compute, std::nullopt, std::nullopt, memory},
        localDevice->ComputeTarget((packet[4] & 0x8000u) != 0 ? 32u : 64u),
        {0, 0, 0, 128}
    };
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    static double captureMs = 0, keyMs = 0, recompileMs = 0, deviceMs = 0;
    static std::uint64_t cacheHits = 0;
    static std::uint64_t dispatches = 0;
    auto lap = std::chrono::steady_clock::now();

    std::array<double, DriverPhaseCount> phaseMs{};
    auto phaseLap = lap;
    DispatchPhaseTiming phaseTiming{profile, lap, phaseMs, phaseLap};
    if (profile && packetStartedAt() != std::chrono::steady_clock::time_point{}) phaseMs[PhasePrologue] = std::chrono::duration<double, std::milli>(lap - packetStartedAt()).count();

    static const bool noDispatchCacheEnv = std::getenv("APS5_NO_DISPATCH_CACHE") != nullptr;

    static const std::pair<std::uint64_t, std::uint64_t> probeDispatch = [] {
        const char* text = std::getenv("APS5_PROBE_DISPATCH");
        if (text == nullptr) return std::pair<std::uint64_t, std::uint64_t>{0, 0};
        char* end = nullptr;
        const auto probeAddress = std::strtoull(text, &end, 16);
        const auto index = end != nullptr && *end == ':' ? std::strtoull(end + 1, nullptr, 10) : 0ull;
        return std::pair<std::uint64_t, std::uint64_t>{probeAddress, index};
    }();
    bool probeThis = false;

    if (probeDispatch.first != 0 && (address & 0xfffffffffull) == (probeDispatch.first & 0xfffffffffull)) {
        static std::atomic<std::uint64_t> dispatchesSeen{0};
        probeThis = dispatchesSeen.fetch_add(1) == probeDispatch.second;
        if (probeThis) std::fprintf(stderr, "[gpu] probing dispatch %llu of 0x%llx\n", static_cast<unsigned long long>(probeDispatch.second), static_cast<unsigned long long>(address));
    }

    if (FailureMemo() && snapshot.handles->poisoned.load(std::memory_order_relaxed) != 0) {
        const std::string* poisoned = nullptr;
        if (SourceHandleFor(snapshot, codeOffset, localDevice->Serial(), request, probeThis, &poisoned) == nullptr && poisoned != nullptr) {
            pendingDispatchPhases().outcome = DispatchOutcome::SkippedMemo;
            return;
        }
    }
    const bool noDispatchCache = noDispatchCacheEnv || probeThis;
    std::uint64_t key = 0xcbf29ce484222325ull;
    const auto mix = [&](std::uint64_t value) {
        key ^= value;
        key *= 0x100000001b3ull;
    };
    mix(address);
    mix(packet[4] & 0x8000u);
    for (const auto threads : compute.partialThreads) mix(threads);
    for (const auto word : userData) mix(word);

    static const bool keyHygiene = std::getenv("APS5_NO_DISPATCH_KEY_HYGIENE") == nullptr;
    if (keyHygiene) {
        for (const auto offset : {0x207u, 0x208u, 0x209u, 0x212u, 0x213u}) {
            const auto found = queue.shader.find(offset);

            mix(found == queue.shader.end() ? (1ull << 32u) : found->second);
        }
    } else {
        for (const auto& [offset, value] : queue.shader) {
            mix(offset);
            mix(value);
        }
    }
    std::shared_ptr<const ShaderRecompiler::RecompileResult> compiledResult;

    std::shared_ptr<DispatchVariant> keepVariant;

    std::shared_ptr<DispatchVariant> attachVariant;

    std::shared_ptr<ShaderMemory> shaderMemory;
    std::vector<ShaderRecompiler::MemoryRegion> captured;

    std::vector<std::uint32_t> liveWords;
    bool dataHit = false;
    bool cached = false;
    bool validated = false;

    std::shared_ptr<DispatchEntry> missedEntry;
    bool missedDiffering = false;
    std::shared_ptr<const ShaderRecompiler::ResourceCapture> capture;

    static const bool traceCache = std::getenv("APS5_TRACE_DISPATCH_CACHE") != nullptr;
    if (traceCache) {
        std::lock_guard traceLock(dispatchCacheMutex);
        struct Last { std::vector<std::uint32_t> userData; std::map<std::uint32_t, std::uint32_t> shader; std::uint64_t key; };
        static std::map<std::uint64_t, Last> last;
        static int reports = 0;
        auto& previous = last[address];
        if (previous.key != 0 && previous.key != key && reports < 200) {
            std::string what;
            for (std::size_t i = 0; i < userData.size(); ++i) {
                if (i >= previous.userData.size() || previous.userData[i] != userData[i]) {
                    char text[48];
                    std::snprintf(text, sizeof(text), " user[%zu] %08x->%08x", i, i < previous.userData.size() ? previous.userData[i] : 0u, userData[i]);
                    what += text;
                }
            }
            for (const auto& [offset, value] : queue.shader) {
                const auto old = previous.shader.find(offset);
                if (old == previous.shader.end() || old->second != value) {
                    char text[48];
                    std::snprintf(text, sizeof(text), " sh[%x] %08x->%08x", offset, old == previous.shader.end() ? 0u : old->second, value);
                    what += text;
                }
            }
            ++reports;
            std::fprintf(stderr, "[dispatch-cache] 0x%llx key changed:%s\n", static_cast<unsigned long long>(address), what.c_str());
        }
        previous.userData = userData;
        previous.shader = std::map<std::uint32_t, std::uint32_t>(queue.shader.begin(), queue.shader.end());
        previous.key = key;
    }

    if (!stampValidate()) mix(reinterpret_cast<std::uintptr_t>(it->second.get()));
    phaseTiming.Phase(PhaseKey);
    lookupDispatch(address, submission, key, noDispatchCache, traceCache, profile, memory, phaseTiming, phaseMs, compiledResult, keepVariant, captured, liveWords, dataHit, cached, validated, missedEntry, missedDiffering);
    if (cached) {
        captureMs += phaseTiming.Elapsed();
    } else {
        shaderMemory = std::make_shared<ShaderMemory>(memory, &queryPendingWrite, &observePendingWrite, hookWaitCounter());
        std::uint64_t forgetAtCapture = 0;

        static const bool dumpShaders = std::getenv("APS5_DUMP_SHADERS") != nullptr;
        try {

            struct ProbeScope {
                bool active;
                explicit ProbeScope(bool active) : active(active) { if (active) ShaderRecompiler::SetDebugProbeActive(true); }
                ~ProbeScope() { if (active) ShaderRecompiler::SetDebugProbeActive(false); }
            } probeScope{probeThis};
            const auto waitedBefore = traceCapSync() ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
            forgetAtCapture = GuestMemory::ForgetSerial();
            const auto handle = SourceHandleFor(snapshot, codeOffset, localDevice->Serial(), request, probeThis);
            capture = [&] {
                const SampledReadScope sampling(evidenceReads);
                return shaderMemory->Capture(request, handle.get());
            }();
            captured = shaderMemory->Regions();
            request.context.memory = captured;
            if (traceCapSync()) traceCapture("dispatch-capture", address, submission.queue, captured, Graphics::Recorder::ThreadWaitedMs() - waitedBefore);
            captureMs += phaseTiming.Elapsed();
            phaseTiming.Phase(PhaseCapture);
            if (dumpShaders) static_cast<void>(dumpRequest(address, request));
            const auto started = std::chrono::steady_clock::now();

            static const bool reuseCapture = std::getenv("APS5_NO_CAPTURE_REUSE") == nullptr;
            bool memoHit = false;
            compiledResult = reuseCapture ? ShaderRecompiler::Recompile(request, *capture, &memoHit) : std::make_shared<const ShaderRecompiler::RecompileResult>(ShaderRecompiler::Recompile(request));
            if (compiledResult->cacheHit || memoHit) ++cacheHits;
            const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            static double totalMs = 0;
            totalMs += elapsed;
            if (profile && elapsed > 200) std::fprintf(stderr, "[gpu] compute shader 0x%llx recompile took %.0f ms (%zu SPIR-V words, %zu captured regions, total %.1f s)\n", static_cast<unsigned long long>(address), elapsed, compiledResult->spirv.size(), captured.size(), totalMs / 1000);
        } catch (const std::exception& error) {
            const auto dump = dumpShaders ? dumpRequest(address, request) : std::string{};

            std::string reason = error.what();
            if (const auto newline = reason.find('\n'); newline != std::string::npos) reason.resize(newline);
            char where[96];
            if (dump.empty()) std::snprintf(where, sizeof(where), "compute shader 0x%llx: ", static_cast<unsigned long long>(address));
            else std::snprintf(where, sizeof(where), "compute shader 0x%llx (%s): ", static_cast<unsigned long long>(address), dump.c_str());
            throw std::runtime_error(where + reason);
        }
        recompileMs += phaseTiming.Elapsed();
        phaseTiming.Phase(PhaseRecompile);
        insertDispatch(address, key, noDispatchCache, profile, it->second, forgetAtCapture, memory, shaderMemory, captured, capture, compiledResult, missedEntry, missedDiffering, attachVariant, phaseTiming);
    }
    if (verifyDataHits() && dataHit) verifyDataHit(snapshot, codeOffset, localDevice->Serial(), request, memory, address, *keepVariant, liveWords, *compiledResult);

    if (recordQueuedLabelsAfterCapture(submission.queue, captured)) {
        dispatch(queue, packet, submission, indirectArguments);
        return;
    }
    phaseTiming.Phase(PhaseQueuedLabels);
    const auto& compiled = *compiledResult;
    std::vector<Graphics::GuestMemorySnapshot> snapshots;
    for (const auto& region : captured) snapshots.push_back({region.guestAddress, region.bytes});
    std::array<std::uint32_t, 3> groups{packet[1], packet[2], packet[3]};
    if (indirectArguments == 0 && (packet[4] & 0x20u) != 0) {

        for (std::uint32_t axis = 0; axis < 3; ++axis) {
            const auto threads = std::max(readRegister(queue.shader, 0x207 + axis) & 0xffffu, 1u);
            groups[axis] = (groups[axis] + threads - 1) / threads;
        }
    }
    static const bool traceIo = std::getenv("APS5_TRACE_DISPATCH_IO") != nullptr;
    if (traceIo) {

        std::string words;
        if (std::getenv("APS5_TRACE_DISPATCH_IO")[0] == '2') {
            for (const auto word : userData) {
                char text[12];
                std::snprintf(text, sizeof(text), " %08x", word);
                words += text;
            }
        }
        std::fprintf(stderr, "[dispatch-io] shader 0x%llx%s\n", static_cast<unsigned long long>(address), words.c_str());
    }

    const auto rethrow = [&](const std::exception& error) {
        char where[64];
        std::snprintf(where, sizeof(where), "compute shader 0x%llx: ", static_cast<unsigned long long>(address));
        throw std::runtime_error(where + std::string(error.what()));
    };
    phaseTiming.Phase(PhaseSnapshots);

    std::shared_ptr<RecipeHit> recipeHit;
    if (cached && keepVariant != nullptr && !stampValidate()) {
        recipeHit = localDevice->PrepareRecipe(keepVariant->recipe.load(std::memory_order_acquire), indirectArguments != 0);
        phaseTiming.Phase(PhaseRecipePrecheck);
    }

    std::shared_ptr<const Recipe> builtRecipe;
    auto* const attachTo = stampValidate() ? nullptr : keepVariant != nullptr ? keepVariant.get() : attachVariant.get();
    bool writersNoted = false;
    const bool noteWrites = writeEvidenceEnabled() || traceCapSync();

    for (;;) {

        std::shared_ptr<PreparedDispatch> prepared;
        if (recipeHit == nullptr || VulkanDevice::VerifyRecipes()) {
            try {
                prepared = localDevice->PrepareDispatch(compiled, snapshots);
            } catch (const std::exception& error) {
                rethrow(error);
            }
            if (profile) {
                const auto now = std::chrono::steady_clock::now();
                const auto prepareMs = std::chrono::duration<double, std::milli>(now - phaseLap).count();
                phaseLap = now;
                double parts = 0;
                if (prepared != nullptr) {
                    const auto phases = VulkanDevice::PreparePhaseMs(*prepared);
                    phaseMs[PhasePrepareKey] += phases[0];
                    phaseMs[PhasePrepareFind] += phases[1];
                    phaseMs[PhasePreparePrecollect] += phases[2];
                    phaseMs[PhasePreparePresync] += phases[3];
                    phaseMs[PhasePrepareStageA] += phases[4];
                    for (const auto part : phases) parts += part;
                }
                phaseMs[PhasePrepareOther] += std::max(0.0, prepareMs - parts);
            }
        }
        GuestMemory::TagGpuLockSite(indirectArguments != 0 ? GuestMemory::GpuLockSite::Indirect : GuestMemory::GpuLockSite::Dispatch);
        std::lock_guard gpuLock(GuestMemory::GpuMutex());
        phaseTiming.Phase(PhaseLockWait);

        recordLabelsForPacket(localDevice.get(), submission.queue);
        phaseTiming.Phase(PhaseLabels);
        if (noteWrites && writerKeyedEvidence() && !writersNoted) {
            noteWrittenBuffers(address, submission.queue, compiled);
            writersNoted = true;
        }
        phaseTiming.Phase(PhaseNoteWriters);
        try {
            if (recipeHit != nullptr) {
                VulkanDevice::IndirectOutcome outcome{0, 0};
                const auto result = localDevice->DispatchRecipe(compiled, groups[0], groups[1], groups[2], indirectArguments, address, recipeHit, outcome, VulkanDevice::VerifyRecipes() ? prepared : nullptr, dataHit);
                if (result == RecipeOutcome::Rebuild) {

                    recipeHit = nullptr;
                    VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Restart, indirectArguments != 0);
                    continue;
                }
                if (indirectArguments != 0) countIndirect(outcome.cpuReason, outcome.argumentReadMs);
            } else if (indirectArguments != 0) {
                const auto outcome = localDevice->DispatchIndirect(compiled, indirectArguments, snapshots, address, std::move(prepared), attachTo != nullptr ? &builtRecipe : nullptr);
                countIndirect(outcome.cpuReason, outcome.argumentReadMs);
            } else {
                localDevice->Dispatch(compiled, groups[0], groups[1], groups[2], snapshots, address, std::move(prepared), attachTo != nullptr ? &builtRecipe : nullptr);
            }
        } catch (const std::exception& error) {
            rethrow(error);
        }
        if (builtRecipe != nullptr) {
            if (dataHit) {

                auto own = std::make_shared<Recipe>(*builtRecipe);
                own->dataWordsHash = Graphics::ShaderResources::DataWordsHash({ShaderRecompiler::ShaderStage::Compute, keepVariant->compiled.get(), 0});
                builtRecipe = std::move(own);
            }
            attachTo->recipe.store(std::move(builtRecipe), std::memory_order_release);
            VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Attach, indirectArguments != 0);
        }
        break;
    }
    phaseTiming.Phase(PhaseDevice);
    if (noteWrites && !writerKeyedEvidence()) noteWrittenBuffers(address, submission.queue, compiled);
    deviceMs += phaseTiming.Elapsed();
    phaseTiming.Phase(PhaseTail);
    if (profile) {
        auto& pending = pendingDispatchPhases();
        pending.outcome = DispatchOutcome::Real;
        pending.phases = true;
        pending.hit = cached;
        pending.validated = validated;
        pending.ms = phaseMs;
        pending.tailAt = phaseLap;
    }
    if (profile && ++dispatches % 100 == 0) std::fprintf(stderr, "[gpu] %llu dispatches (%llu dispatch cache hits, %llu evictions, %llu recompile cache hits): capture %.1f s, cache key %.1f s, recompile %.1f s, device %.1f s\n", static_cast<unsigned long long>(dispatches), static_cast<unsigned long long>(dispatchCacheHits), static_cast<unsigned long long>(dispatchCacheEvictions), static_cast<unsigned long long>(cacheHits), captureMs / 1000, keyMs / 1000, recompileMs / 1000, deviceMs / 1000);
}

}
