#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/Submission.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/SynchronizationStatistics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Capture/Replay.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Capture/FrameCapture.hpp"
#include "ThreadOwned.hpp"
#include <bit>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace AgcDriver::DriverDetail {

std::deque<const std::uint32_t*>& Driver::releasedTails() {
    static thread_local std::deque<const std::uint32_t*> released;
    return released;
}

void Driver::noteReleasedTails(const Submission& submission) {
    auto& released = releasedTails();
    released.clear();
    thread_local Submission* scratchSlot = nullptr;
    auto& scratch = ShaderRecompiler::ThreadOwned(scratchSlot);
    const std::uint32_t* tail = submission.rewindTail;
    std::size_t words = submission.rewindWords;
    for (std::size_t chunk = 0; chunk < 256 && tail != nullptr; ++chunk) {
        if ((std::atomic_ref<std::uint32_t>(*const_cast<std::uint32_t*>(tail - 1)).load(std::memory_order_acquire) & 0x80000000u) == 0) break;
        released.push_back(tail - 1);
        scratch.rewindTail = nullptr;
        scratch.rewindWords = 0;
        try {
            copyCommands(scratch, tail, words);
        } catch (const std::exception&) {
            break;
        }
        tail = scratch.rewindTail;
        words = scratch.rewindWords;
    }
}

void Driver::copyCommands(Submission& submission, const std::uint32_t* guest, std::size_t words) {
    submission.commands.clear();
    std::size_t budget = std::size_t{1} << 26u;
    copySegment(submission, guest, words, budget);
}

bool Driver::copySegment(Submission& submission, const std::uint32_t* guest, std::size_t words, std::size_t& budget) {
    require(words <= budget, "command buffer jumps exceed the copy limit (a jump loop?)");
    budget -= words;
    for (std::size_t cursor = 0; cursor < words;) {
        const auto header = guest[cursor];
        if (Pm4::FillerPacket(header)) { submission.commands.push_back(header); ++cursor; continue; }
        const auto count = (header & 0xc0000000u) == 0xc0000000u ? Pm4::PacketWords(header) : words - cursor;
        if ((header & 0xc0000000u) != 0xc0000000u || count > words - cursor) {
            submission.commands.insert(submission.commands.end(), guest + cursor, guest + words);
            return false;
        }
        const auto opcode = (header >> 8u) & 0xffu;
        if (opcode == 0x3fu) {
            require(count == 4, "invalid INDIRECT_BUFFER size");
            const auto* target = reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(guest[cursor + 1] & ~3u) | (static_cast<std::uintptr_t>(guest[cursor + 2] & 0xffffu) << 32u));
            const std::size_t targetWords = guest[cursor + 3] & 0xfffffu;
            const bool chain = (guest[cursor + 3] & (1u << 20u)) != 0;
            GuestMemory::CheckRange(target, targetWords * sizeof(std::uint32_t), alignof(std::uint32_t));
            if (copySegment(submission, target, targetWords, budget)) return true;
            if (chain) return false;
            cursor += count;
            continue;
        }
        submission.commands.insert(submission.commands.end(), guest + cursor, guest + cursor + count);
        cursor += count;
        if (opcode == 0x59u) {
            submission.rewindTail = guest + cursor;
            submission.rewindWords = words - cursor;
            return true;
        }
    }
    return false;
}

void Driver::waitForFlipRoom(const Submission& submission) {
    for (std::size_t cursor = 0; cursor < submission.commands.size(); cursor += Pm4::PacketWords(submission.commands[cursor])) {
        if (submission.commands[cursor] != FlipPacketHeader) continue;
        std::shared_ptr<IVideoOutput> output;
        {
            std::lock_guard lock(mutex);
            const auto found = outputs.find(submission.commands[cursor + 1]);
            require(found != outputs.end(), "flip references an unregistered video output");
            output = found->second;
        }
        output->WaitForFlipRoom();
    }
}

void Driver::reserveOutputs(Submission& submission) {
    for (std::size_t cursor = 0; cursor < submission.commands.size();) {
        const auto* words = submission.commands.data() + cursor;
        if (words[0] == RenderingWaitPacketHeader) {
            const auto output = outputs.find(words[1]);
            require(output != outputs.end(), "rendering wait references an unregistered video output");
            auto wait = output->second->CaptureRenderingWait(words[2]);
            require(wait != nullptr, "video output returned a null rendering wait");
            submission.renderingWaits.emplace(cursor, std::move(wait));
        }
        noteHeldAtSubmit(submission, cursor);
        if (words[0] == FlipPacketHeader) {
            const auto output = outputs.find(words[1]);
            require(output != outputs.end(), "flip references an unregistered video output");
            const FlipInfo info{words[1], std::bit_cast<std::int32_t>(words[2]), words[3], std::bit_cast<std::int64_t>(static_cast<std::uint64_t>(words[4]) | (static_cast<std::uint64_t>(words[5]) << 32u))};
            auto request = output->second->Reserve(info);
            require(request != nullptr, "video output returned a null flip reservation");
            submission.flips.emplace(cursor, std::move(request));
        }
        cursor += Pm4::PacketWords(words[0]);
    }
}

void Driver::executeRewindTail(const Submission& stalled) {
    // A replay loop's end abandons a REWIND waiting for commands past the capture.
    const auto generation = Capture::ReplayRewindGeneration();
    std::atomic_ref<std::uint32_t> control(*const_cast<std::uint32_t*>(stalled.rewindTail - 1));
    if ((control.load(std::memory_order_acquire) & 0x80000000u) == 0) {
        noteWaitBlocked(stalled.queue, reinterpret_cast<std::uint64_t>(stalled.rewindTail - 1), true);
        struct BlockedWait {
            Driver& driver;
            std::uint32_t queue;
            ~BlockedWait() {
                driver.holdForCapture();
                driver.noteWaitBlocked(queue, 0, false);
            }
        } blocked{*this, stalled.queue};
        while ((control.load(std::memory_order_acquire) & 0x80000000u) == 0) {
            CheckFailure();
            checkStopping();
            if (Capture::ReplayRewindAbandoned(generation)) return;
            PollSleep();
        }
    }
    Submission tail{};
    tail.queue = stalled.queue;
    auto& released = releasedTails();
    if (!released.empty() && released.front() == stalled.rewindTail - 1) {
        released.pop_front();
        tail.enqueuedAt = lastEpochBump();
    } else {
        released.clear();
        tail.enqueuedAt = std::chrono::steady_clock::now();
    }
    // A replay runs the words the game's driver copied at this release (its memory deltas are coarser
    // than the game's hand-off, so the chunk in guest memory may already hold later commands).
    std::uint64_t nextTail = 0;
    std::uint64_t nextWords = 0;
    const auto fed = Capture::ReplayTakeRewindTail(stalled.queue, generation, tail.commands, nextTail, nextWords, [&] { CheckFailure(); checkStopping(); });
    if (fed == Capture::RewindFeed::Abandoned) return;
    if (fed == Capture::RewindFeed::Taken) {
        tail.rewindTail = reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(nextTail));
        tail.rewindWords = static_cast<std::size_t>(nextWords);
    } else {
        // A replay without recorded tails may be writing the delta that released this REWIND.
        std::shared_lock replayWrites(Capture::ReplayMemoryWriteMutex());
        copyCommands(tail, stalled.rewindTail, stalled.rewindWords);
    }
    if (auto& capture = Capture::FrameCapture::Get(); capture.Recording()) {
        // The CPU writes this tail's commands depend on are recorded with it, so the replay's memory is
        // what the game's GPU saw here. The submitting thread may hold the capture lock while it waits
        // for this queue (flip room): the delta is then left to its next submission.
        std::unique_lock captureLock(capture.SubmitMutex(), std::try_to_lock);
        if (captureLock.owns_lock()) {
            try {
                capture.RecordDelta(captureProgress());
            } catch (const std::exception& error) {
                captureFailed(error);
            }
        }
        capture.RecordRewindTail({tail.queue, 0, reinterpret_cast<std::uintptr_t>(tail.rewindTail), tail.rewindWords}, tail.commands);
    }
    validate(tail.commands, tail.queue, stalled.rewindTail);
    waitForFlipRoom(tail);
    {
        std::lock_guard lock(mutex);
        rethrowFailure();
        checkStopping();
        reserveOutputs(tail);
        tail.shaders = shaders;
        tail.serial = stalled.serial;
        tail.received = ++eventSerial;
    }
    execute(tail);
}

void Driver::Submit(const Packet* packet, std::uint32_t queue) {
    CheckFailure();
    require(queue == 0 || (queue >= 0x20 && queue < 0x58), "unsupported compute queue");
    GuestMemory::CheckRange(packet, sizeof(Packet), alignof(Packet));
    const auto descriptor = *packet;
    require(descriptor.flags == 0, "nonzero submission flags are not implemented");
    auto& capture = Capture::FrameCapture::Get();
    std::unique_lock captureLock(capture.SubmitMutex(), std::defer_lock);
    if (capture.Active()) {
        captureLock.lock();
        captureBeforeSubmit();
    }
    Submission submission{};
    submission.queue = queue;
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    auto& costs = submissionCosts(queue);
    const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (descriptor.dw_num != 0) {
        require(descriptor.dw_num <= std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t), "command size overflow");
        GuestMemory::CheckRange(descriptor.addr, static_cast<std::size_t>(descriptor.dw_num) * sizeof(std::uint32_t), alignof(std::uint32_t));
        copyCommands(submission, descriptor.addr, descriptor.dw_num);
        submission.source = static_cast<const std::uint32_t*>(descriptor.addr);
    }
    const auto copied = profile ? std::chrono::steady_clock::now() : start;
    checkReplayCommands(submission.commands);
    validate(submission.commands, queue, descriptor.addr);
    waitForFlipRoom(submission);
    static const bool trace = std::getenv("APS5_TRACE_GPU") != nullptr;
    if (trace) std::fprintf(stderr, "[gpu] %.1f submit queue=0x%x dwords=%zu at %p\n", TraceMs(), queue, submission.commands.size(), static_cast<const void*>(descriptor.addr));
    const auto validated = profile ? std::chrono::steady_clock::now() : start;
    {
        std::lock_guard lock(mutex);
        rethrowFailure();
        checkStopping();
        require(accepted != std::numeric_limits<std::uint64_t>::max(), "submission serial overflow");
        reserveOutputs(submission);
        submission.shaders = shaders;
        submission.serial = accepted + 1;

        submission.received = ++eventSerial;
        const auto now = std::chrono::steady_clock::now();
        submission.enqueuedAt = now;
        if (profile) {
            ++costs.submissions;
            costs.validateNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(validated - copied).count());
            costs.copyNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>((copied - start) + (now - validated)).count());
        }
        if (captureLock.owns_lock()) captureSubmitted(submission, descriptor);
        enqueue(std::move(submission));
        ++accepted;
    }
    changed.notify_all();
    if (captureLock.owns_lock()) captureAfterSubmit();
}

void Driver::SuspendPoint() {
    require(!onWorkerThread(), "worker cannot suspend itself");
    auto& capture = Capture::FrameCapture::Get();
    std::unique_lock captureLock(capture.SubmitMutex(), std::defer_lock);
    if (capture.Active()) captureLock.lock();
    std::unique_lock lock(mutex);
    rethrowFailure();
    checkStopping();
    require(accepted != std::numeric_limits<std::uint64_t>::max(), "submission serial overflow");
    if (captureLock.owns_lock() && capture.Recording()) {
        try {
            capture.RecordEvent(Capture::EventType::Suspend, {});
        } catch (const std::exception& error) {
            captureFailed(error);
        }
    }
    Submission boundary{};
    boundary.serial = accepted + 1;
    boundary.suspend = true;

    boundary.queue = 0;
    boundary.enqueuedAt = std::chrono::steady_clock::now();
    enqueue(std::move(boundary));
    ++accepted;

    changed.notify_all();
}

bool Driver::waitFree(const Submission& submission) {
    if (submission.queue == 0 || submission.suspend || !submission.flips.empty() || !submission.renderingWaits.empty() || submission.rewindTail != nullptr) return false;
    for (std::size_t at = 0; at < submission.commands.size(); at += std::max<std::size_t>(1, Pm4::PacketWords(submission.commands[at]))) {
        const auto header = submission.commands[at];
        const auto opcode = (header >> 8u) & 0xffu;
        if ((header >> 30u) == 3u && (opcode == 0x3c || opcode == 0x93)) return false;
    }
    return true;
}

bool Driver::queue0Before(std::uint64_t received) const {
    if (queue0Executing != 0 && queue0Executing < received) return true;
    if (!queue0Uncommitted.empty() && queue0Uncommitted.front() < received) return true;
    const auto worker = workers.find(0);
    if (worker == workers.end()) return false;
    for (const auto& pending : worker->second.pending) {
        if (pending.suspend) continue;
        return pending.received < received;
    }
    return false;
}

bool Driver::orderReleased(std::uint32_t queue, std::uint64_t received) const {
    if (!queue0Before(received)) return true;
    const auto awaited = queue0Awaited.load(std::memory_order_acquire);
    if (awaited == 0) return false;
    if (workers.at(queue).unfinishedWrites.contains(awaited & ~std::uint64_t{3})) return true;
    return runningWorkers.load(std::memory_order_acquire) == 0 && !completionsPending() && !Graphics::Recorder::SnapshotWriteOverlaps(awaited, 4);
}

void Driver::holdForCapture() const noexcept {
    while (captureHold.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::microseconds(200));
}

void Driver::noteWaitBlocked(std::uint32_t queue, std::uint64_t awaited, bool blocked) {
    queueBlocked[queue].store(blocked, std::memory_order_release);
    queueAwaited[queue].store(blocked ? awaited : 0, std::memory_order_release);
    if (blocked) runningWorkers.fetch_sub(1, std::memory_order_acq_rel);
    else runningWorkers.fetch_add(1, std::memory_order_acq_rel);
    if (queue == 0) queue0Awaited.store(blocked ? awaited : 0, std::memory_order_release);
    if (blocked && orderHolders.load(std::memory_order_acquire) != 0) {
        std::lock_guard lock(mutex);
        changed.notify_all();
    }
}

void Driver::enqueue(Submission submission) {
    const auto queue = submission.queue;
    submission.waitFree = waitFree(submission);
    auto& worker = workers[queue];
    for (const auto dword : submission.labelWrites) ++worker.unfinishedWrites[dword];
    worker.pending.push_back(std::move(submission));
    worker.queued.fetch_add(1, std::memory_order_acq_rel);
    if (!worker.thread.joinable()) worker.thread = std::thread([this, queue] { run(queue); });
}

void Driver::noteHeldAtSubmit(Submission& submission, std::size_t cursor) {
    const auto packet = std::span<const std::uint32_t>(submission.commands).subspan(cursor, std::min<std::size_t>(Pm4::PacketWords(submission.commands[cursor]), submission.commands.size() - cursor));
    const auto opcode = (packet[0] >> 8u) & 0xffu;
    if ((packet[0] >> 30u) != 3u) return;
    if (opcode == 0x49 || opcode == 0x37) {
        if (const auto label = Pm4::DecodeLabelWrite(packet)) {
            const auto bytes = label->Bytes();
            if (label->address % 4 == 0 && bytes.size() <= 64) {
                for (std::size_t offset = 0; offset < bytes.size(); offset += 4) submission.labelWrites.push_back(label->address + offset);
            }
        }
        return;
    }
    if ((opcode != 0x3c && opcode != 0x93) || packet.size() < 7 || ((packet[1] >> 4u) & 3u) != 1u) return;
    const auto address = packet[2] | (static_cast<std::uint64_t>(packet[3]) << 32u);
    const std::size_t bytes = Pm4::WaitAwaitedBytes(packet);
    if (address % 4 != 0 || !GuestMemory::Accessible(reinterpret_cast<const void*>(address), bytes)) return;
    const auto worker = workers.find(submission.queue);
    for (std::size_t offset = 0; offset < bytes; offset += 4) {
        const auto dword = address + offset;
        if (std::find(submission.labelWrites.begin(), submission.labelWrites.end(), dword) != submission.labelWrites.end()) return;
        if (worker != workers.end() && worker->second.unfinishedWrites.contains(dword)) return;
    }
    std::uint64_t value = *reinterpret_cast<const volatile std::uint32_t*>(address);
    if (bytes == 8) value |= static_cast<std::uint64_t>(*reinterpret_cast<const volatile std::uint32_t*>(address + 4)) << 32u;
    if (Pm4::WaitComparesValue(packet, value)) submission.heldAtSubmit.insert(cursor);
}

void Driver::forgetUnfinishedWrites(QueueWorker& worker, const Submission& submission) {
    forgetUnfinishedWrites(worker, submission.labelWrites);
}

void Driver::forgetUnfinishedWrites(QueueWorker& worker, std::span<const std::uint64_t> labelWrites) {
    for (const auto dword : labelWrites) {
        const auto found = worker.unfinishedWrites.find(dword);
        if (found == worker.unfinishedWrites.end()) continue;
        if (--found->second == 0) worker.unfinishedWrites.erase(found);
    }
}

}
