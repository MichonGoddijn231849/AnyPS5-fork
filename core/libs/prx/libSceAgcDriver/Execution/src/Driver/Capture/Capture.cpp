#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Capture/Replay.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include <cstring>
#include <optional>

namespace AgcDriver::DriverDetail {

std::optional<std::uint64_t>& ExpectedCommands() {
    static thread_local std::optional<std::uint64_t> expected;
    return expected;
}

std::atomic<std::uint64_t> commandMismatches{0};

void Driver::checkReplayCommands(std::span<const std::uint32_t> commands) {
    auto& expected = ExpectedCommands();
    if (!expected) return;
    const auto hash = *expected;
    expected.reset();
    if (Capture::HashWords(commands) == hash) return;
    if (commandMismatches.fetch_add(1, std::memory_order_relaxed) < 8) std::fprintf(stderr, "[replay] submitted commands differ from the captured ones (%zu words): guest memory was not restored exactly\n", commands.size());
}

namespace {

void PutRegisters(Capture::Writer& writer, const Registers& registers) {
    std::vector<std::uint32_t> pairs;
    pairs.reserve(registers.size() * 2);
    for (const auto& [offset, value] : registers) {
        pairs.push_back(offset);
        pairs.push_back(value);
    }
    writer.PutSpan<std::uint32_t>(pairs);
}

Registers GetRegisters(Capture::Reader& reader) {
    const auto pairs = reader.GetSpan<std::uint32_t>();
    if (pairs.size() % 2 != 0) throw std::runtime_error("capture register list has an odd length");
    Registers registers;
    for (std::size_t i = 0; i < pairs.size(); i += 2) registers.insert_or_assign(pairs[i], pairs[i + 1]);
    return registers;
}

}

std::vector<std::byte> Driver::serializeQueueState(const QueueState& state) {
    Capture::Writer writer;
    PutRegisters(writer, state.shader);
    PutRegisters(writer, state.context);
    PutRegisters(writer, state.userConfig);
    writer.Put<std::uint8_t>(state.savedContext.has_value());
    if (state.savedContext) PutRegisters(writer, *state.savedContext);
    writer.PutSpan<std::uint32_t>(state.constantRam);
    writer.Put(state.indexBase);
    writer.Put(state.drawIndirectBase);
    writer.Put(state.dispatchIndirectBase);
    writer.Put(state.indexBufferSize);
    writer.Put(state.indexType);
    writer.Put(state.instanceCount);
    writer.Put<std::uint64_t>(state.markers.size());
    for (const auto& marker : state.markers) writer.PutString(marker);
    return std::move(writer.data);
}

QueueState Driver::deserializeQueueState(std::span<const std::byte> bytes) {
    Capture::Reader reader(bytes);
    QueueState state;
    state.shader = GetRegisters(reader);
    state.context = GetRegisters(reader);
    state.userConfig = GetRegisters(reader);
    if (reader.Get<std::uint8_t>() != 0) state.savedContext = GetRegisters(reader);
    const auto ram = reader.GetSpan<std::uint32_t>();
    require(ram.size() == state.constantRam.size(), "capture constant RAM size differs");
    std::copy(ram.begin(), ram.end(), state.constantRam.begin());
    state.indexBase = reader.Get<std::uint64_t>();
    state.drawIndirectBase = reader.Get<std::uint64_t>();
    state.dispatchIndirectBase = reader.Get<std::uint64_t>();
    state.indexBufferSize = reader.Get<std::uint32_t>();
    state.indexType = reader.Get<std::uint32_t>();
    state.instanceCount = reader.Get<std::uint32_t>();
    const auto markers = reader.Get<std::uint64_t>();
    for (std::uint64_t i = 0; i < markers; ++i) state.markers.push_back(reader.GetString());
    require(reader.Done(), "capture queue state has trailing bytes");
    return state;
}

void Driver::captureFailed(const std::exception& error) {
    Capture::FrameCapture::Get().Fail(error.what());
}

std::vector<std::uint64_t> Driver::captureProgress() const {
    std::vector<std::uint64_t> progress(Capture::QueueCount);
    for (std::size_t queue = 0; queue < progress.size(); ++queue) progress[queue] = packetsExecuted[queue].load(std::memory_order_acquire) - progressBase[queue];
    return progress;
}

void Driver::captureShader(const ShaderSnapshot& snapshot) {
    Capture::FrameCapture::Get().RecordShader(snapshot.codeAddress, snapshot.headerAddress, snapshot.type, snapshot.code, snapshot.header);
}

bool Driver::DrainFor(std::chrono::milliseconds limit) {
    std::unique_lock lock(mutex);
    ++idleWaiters;
    const bool idle = changed.wait_for(lock, limit, [&] { return failure != nullptr || stopping || completed >= accepted; });
    --idleWaiters;
    rethrowFailure();
    checkStopping();
    return idle;
}

void Driver::Settle() {
    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Flush);
    std::lock_guard gpuLock(GuestMemory::GpuMutex());
    if (const auto localDevice = device.Load()) {
        recordDeferredLabels(localDevice.get(), 0);
        localDevice->WaitIdle();
    }
}

std::uint64_t Driver::PacketsExecuted(std::uint32_t queue) const {
    require(queue < Capture::QueueCount, "queue id out of range");
    return packetsExecuted[queue].load(std::memory_order_acquire);
}

bool Driver::Stalled() {
    std::lock_guard lock(mutex);
    if (runningWorkers.load(std::memory_order_acquire) != 0) return false;
    for (const auto& [queue, worker] : workers) {
        if (!worker.pending.empty() && !queueBlocked[queue].load(std::memory_order_acquire)) return false;
    }
    return true;
}

void Driver::RestoreQueueState(std::uint32_t queue, std::span<const std::byte> state) {
    auto restored = deserializeQueueState(state);
    std::lock_guard lock(mutex);
    require(completed >= accepted, "queue state restored while submissions are pending");
    queues[queue] = std::move(restored);
}

void Driver::RestoreDriverState(bool reset, std::span<const std::byte> gds) {
    // This branch does not emulate GDS; a capture carries none.
    std::lock_guard lock(mutex);
    require(completed >= accepted, "driver state restored while submissions are pending");
    resetGraphics = reset;
}

void Driver::ClearCaches(std::uint32_t classes) {
    {
        std::lock_guard lock(mutex);
        require(completed >= accepted, "caches cleared while submissions are pending");
    }
    if ((classes & Capture::CacheDispatch) != 0) {
        std::lock_guard lock(dispatchCacheMutex);
        dispatchCache.clear();
        dispatchOrder.clear();
        priorValueSets.clear();
        dispatchCacheVariants = 0;
        dispatchCacheVariantBytes = 0;
    }
    if ((classes & Capture::CacheDraw) != 0) {
        std::lock_guard lock(drawCacheMutex);
        drawCache.clear();
        drawShapes.clear();
        drawOrder.clear();
        drawCacheVariants = 0;
        drawCacheVariantBytes = 0;
    }
    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Flush);
    std::lock_guard gpuLock(GuestMemory::GpuMutex());
    if (const auto localDevice = device.Load()) localDevice->DropCaches((classes & Capture::CacheResources) != 0, (classes & Capture::CacheTextures) != 0, (classes & Capture::CacheTables) != 0, (classes & Capture::CacheSpace) != 0);
}

bool Driver::captureStart() {
    auto& capture = Capture::FrameCapture::Get();
    if (!DrainFor(std::chrono::seconds(3))) {
        std::fprintf(stderr, "[frame-capture] the driver did not go idle within 3 s after flip %llu; retrying after the next flip\n", static_cast<unsigned long long>(capture.flips));
        return false;
    }
    Settle();
    std::fprintf(stderr, "[frame-capture] starting after flip %llu\n", static_cast<unsigned long long>(capture.flips));
    capture.Begin(capture.flips);
    std::vector<std::shared_ptr<const ShaderSnapshot>> registered;
    {
        std::lock_guard lock(mutex);
        require(completed >= accepted, "submissions arrived while the capture started");
        for (const auto& [queue, state] : queues) {
            Capture::Writer writer;
            writer.Put(queue);
            const auto bytes = serializeQueueState(state);
            writer.PutSpan<std::byte>(bytes);
            capture.RecordEvent(Capture::EventType::QueueState, writer.data);
        }
        Capture::Writer driverState;
        driverState.Put<std::uint8_t>(resetGraphics);
        driverState.PutSpan<std::byte>(std::span<const std::byte>{});
        capture.RecordEvent(Capture::EventType::DriverState, driverState.data);
        for (const auto& [handle, output] : outputs) {
            Capture::Writer writer;
            writer.Put(Capture::VideoOutputEvent{handle, 1});
            capture.RecordEvent(Capture::EventType::VideoOutput, writer.data);
        }
        if (shaders) {
            for (const auto& [address, snapshot] : *shaders) registered.push_back(snapshot);
        }
        for (std::size_t queue = 0; queue < progressBase.size(); ++queue) progressBase[queue] = packetsExecuted[queue].load(std::memory_order_acquire);
    }
    for (const auto& snapshot : registered) captureShader(*snapshot);
    return true;
}

void Driver::captureBeforeSubmit() {
    auto& capture = Capture::FrameCapture::Get();
    try {
        if (!capture.Recording()) {
            if (capture.flips < capture.nextAttempt) return;
            if (!captureStart()) {
                capture.nextAttempt = capture.flips + 1;
                return;
            }
        }
        capture.RecordDelta(captureProgress());
    } catch (const ProcessShutdown&) {
        throw;
    } catch (const std::exception& error) {
        captureFailed(error);
    }
}

void Driver::captureSubmitted(const Submission& submission, const Packet& descriptor) {
    auto& capture = Capture::FrameCapture::Get();
    if (capture.Recording()) {
        try {
            const Capture::SubmitEvent event{submission.queue, static_cast<std::uint32_t>(submission.flips.size()), reinterpret_cast<std::uintptr_t>(descriptor.addr), descriptor.dw_num, static_cast<std::uint32_t>(submission.commands.size()), Capture::HashWords(submission.commands)};
            capture.RecordSubmit(event, submission.commands);
            ++capture.submissions;
        } catch (const std::exception& error) {
            captureFailed(error);
        }
    }
    capture.flips += submission.flips.size();
}

void Driver::captureAfterSubmit() {
    auto& capture = Capture::FrameCapture::Get();
    if (capture.Recording() && capture.flips > capture.LastFrame()) captureFinish();
}

void Driver::captureFinish() {
    auto& capture = Capture::FrameCapture::Get();
    try {
        const auto flipsEnd = capture.flips;
        if (!DrainFor(std::chrono::seconds(10))) std::fprintf(stderr, "[frame-capture] the driver did not go idle within 10 s at the end; the final delta may miss late CPU writes\n");
        capture.RecordDelta(captureProgress());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!capture.PresentsReached(flipsEnd) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (!capture.PresentsReached(flipsEnd)) std::fprintf(stderr, "[frame-capture] not every captured flip was presented within 10 s; their display buffers are missing\n");
        capture.Finish({flipsEnd - capture.FlipsBefore(), capture.submissions});
    } catch (const ProcessShutdown&) {
        throw;
    } catch (const std::exception& error) {
        captureFailed(error);
    }
}

}

namespace AgcDriver::Capture {

std::uint64_t ReplayPacketsExecuted(std::uint32_t queue) {
    return DriverDetail::Driver::Get().PacketsExecuted(queue);
}

void ReplayClearCaches(std::uint32_t classes) {
    DriverDetail::Driver::Get().ClearCaches(classes);
}

bool ReplayStalled() {
    return DriverDetail::Driver::Get().Stalled();
}

bool ReplayDrain(std::chrono::milliseconds limit) {
    return DriverDetail::Driver::Get().DrainFor(limit);
}

void ReplaySettle() {
    DriverDetail::Driver::Get().Settle();
}

void ReplayCollectWrites(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges) {
    for (const auto& [begin, end] : ranges) GuestMemory::CollectWritesUncached(begin, static_cast<std::size_t>(end - begin));
}

void ReplayRestoreQueueState(std::uint32_t queue, std::span<const std::byte> state) {
    DriverDetail::Driver::Get().RestoreQueueState(queue, state);
}

void ReplayRestoreDriverState(bool resetGraphics, std::span<const std::byte> gds) {
    DriverDetail::Driver::Get().RestoreDriverState(resetGraphics, gds);
}

void ReplayRegisterShader(std::uint64_t codeAddress, std::uint64_t headerAddress, std::uint8_t type, std::span<const std::uint32_t> code, std::span<const std::byte> header) {
    DriverDetail::Driver::Get().RegisterShaderSnapshot({codeAddress, headerAddress, type, {code.begin(), code.end()}, {header.begin(), header.end()}});
}

void ReplayDumpNextPresent(std::string path, std::uint32_t scale) {
    VulkanDevice::DumpNextPresent(std::move(path), scale);
}

void ReplayExpectCommands(std::uint64_t hash) {
    DriverDetail::ExpectedCommands() = hash;
}

std::uint64_t ReplayCommandMismatches() {
    return DriverDetail::commandMismatches.load(std::memory_order_relaxed);
}

}
