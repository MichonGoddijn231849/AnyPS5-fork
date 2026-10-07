#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/DriverThreadClock.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace AgcDriver::DriverDetail {

namespace {

constexpr std::array<const char*, static_cast<std::size_t>(DrawPipeline::DrainReason::Count)> DrainNames{"packet", "flush", "labels", "capture", "indirect", "submission"};

std::uint64_t elapsedNs(std::chrono::steady_clock::time_point since) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count());
}

}

DrawPipeline& DrawPipeline::Queue0() {
    static DrawPipeline* pipeline = new DrawPipeline();
    return *pipeline;
}

std::size_t DrawPipeline::Depth() {
    static const std::size_t depth = [] {
        const char* text = std::getenv("APS5_PIPELINED_DRAWS");
        if (std::getenv("APS5_LOCKED_DRAW_PREPARE") != nullptr || std::getenv("APS5_DRAW_DRAIN") != nullptr || std::getenv("APS5_DRAIN_ALL") != nullptr || std::getenv("APS5_NO_WORDWISE_CAPTURE") != nullptr) return std::size_t{0};
        if (text == nullptr) return std::size_t{8};
        const auto value = std::strtoull(text, nullptr, 10);
        return value == 0 ? std::size_t{0} : value == 1 ? std::size_t{8} : static_cast<std::size_t>(std::min<unsigned long long>(value, 256));
    }();
    return depth;
}

bool& DrawPipeline::Active() {
    static thread_local bool active = false;
    return active;
}

std::atomic<std::uint64_t>& DrawPipeline::EpochToken() {
    static std::atomic<std::uint64_t> token{1};
    return token;
}

void DrawPipeline::FollowEpoch(std::uint64_t token) {
    static thread_local std::uint64_t followed = 0;
    if (followed == token) return;
    followed = token;
    GuestMemory::BumpCollectEpoch();
}

void DrawPipeline::rethrowFailure() {
    if (failure == nullptr) return;
    auto error = std::exchange(failure, nullptr);
    std::rethrow_exception(error);
}

void DrawPipeline::push(Item item, std::unique_lock<std::mutex>& lock) {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    rethrowFailure();
    if (items.size() >= Depth()) {
        const auto start = std::chrono::steady_clock::now();
        ++idleWaiters;
        idle.wait(lock, [&] { return items.size() < Depth(); });
        --idleWaiters;
        fullWaitNs += elapsedNs(start);
        rethrowFailure();
    }
    depthSum += items.size();
    ++enqueued;
    item.seq = nextSeq++;
    items.push_back(std::move(item));
    outstanding.fetch_add(1, std::memory_order_release);
    if (!thread.joinable()) thread = std::thread([this] { run(); });
    if (committerWaiting) wake.notify_one();
    if (!profile) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport >= std::chrono::seconds(10)) report(now);
}

void DrawPipeline::Enqueue(Commit commit, std::vector<Range> writes, std::uint64_t labelAddress, std::vector<std::byte> labelBytes) {
    std::unique_lock lock(mutex);
    push({std::move(commit), std::move(writes), labelAddress, std::move(labelBytes), 0, nullptr}, lock);
}

std::uint64_t DrawPipeline::EnqueuePending(std::shared_ptr<PendingDraw> pending) {
    std::unique_lock lock(mutex);
    push({Commit{}, {}, 0, {}, 0, std::move(pending)}, lock);
    return items.back().seq;
}

void DrawPipeline::Publish(const std::shared_ptr<PendingDraw>& pending, Commit commit, std::vector<Range> writes) {
    std::lock_guard lock(mutex);
    pending->commit = std::move(commit);
    pending->writes = std::move(writes);
    pending->ready = true;
    if (committerWaiting) wake.notify_one();
    if (idleWaiters != 0) idle.notify_all();
}

std::size_t DrawPipeline::Helpers() {
    static const std::size_t helpers = [] {
        const char* text = std::getenv("APS5_PARALLEL_DRAWS");
        if (text == nullptr || Depth() == 0) return std::size_t{0};
        return static_cast<std::size_t>(std::min<unsigned long long>(std::strtoull(text, nullptr, 10), 8));
    }();
    return helpers;
}

std::uint64_t& DrawPipeline::CurrentSeq() {
    static thread_local std::uint64_t seq = 0;
    return seq;
}

void DrawPipeline::RunOnHelper(std::function<void()> job) {
    std::lock_guard lock(helperMutex);
    if (helpers.empty()) {
        for (std::size_t i = 0; i < Helpers(); ++i) helpers.emplace_back([this] { helperLoop(); });
    }
    helperJobs.push_back(std::move(job));
    helperWake.notify_one();
}

void DrawPipeline::helperLoop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(helperMutex);
            helperWake.wait(lock, [&] { return !helperJobs.empty(); });
            job = std::move(helperJobs.front());
            helperJobs.pop_front();
        }
        job();
    }
}

void DrawPipeline::report(std::chrono::steady_clock::time_point now) {
    lastReport = now;
    std::string drainText;
    for (std::size_t i = 0; i < drains.size(); ++i) {
        char item[64];
        std::snprintf(item, sizeof(item), " %s %llu (%.1f ms)", DrainNames[i], static_cast<unsigned long long>(drains[i]), static_cast<double>(drainWaitNs[i]) / 1e6);
        drainText += item;
    }
    drainText += "; packet drains by opcode:";
    for (int shown = 0; shown < 5; ++shown) {
        const auto top = std::max_element(drainOpcodes.begin(), drainOpcodes.begin() + 0x100);
        if (*top == 0) break;
        char item[32];
        std::snprintf(item, sizeof(item), " 0x%02x x%llu", static_cast<unsigned>(top - drainOpcodes.begin()), static_cast<unsigned long long>(*top));
        drainText += item;
        *top = 0;
    }
    drainOpcodes = {};
    const auto perCommit = commits == 0 ? 0.0 : static_cast<double>(commitNs) / 1e3 / static_cast<double>(commits);
    const auto perDraw = enqueued == 0 ? 0.0 : static_cast<double>(fullWaitNs) / 1e3 / static_cast<double>(enqueued);
    const auto queued = enqueued == 0 ? 0.0 : static_cast<double>(depthSum) / static_cast<double>(enqueued);
    std::fprintf(stderr, "[draw] pipelined (10 s, depth %zu): %llu commits at %.1f us each on the committer, %llu failed; %llu enqueued behind %.2f on average, waiting %.1f us per draw for room; drains:%s\n", Depth(), static_cast<unsigned long long>(commits), perCommit, static_cast<unsigned long long>(commitErrors), static_cast<unsigned long long>(enqueued), queued, perDraw, drainText.c_str());
    commits = commitNs = commitErrors = enqueued = fullWaitNs = depthSum = 0;
    drains = {};
    drainWaitNs = {};
}

void DrawPipeline::run() {
    RegisterDriverThread(DriverThread::Committer);
    for (;;) {
        Commit* commit = nullptr;
        {
            std::unique_lock lock(mutex);
            committerWaiting = true;
            wake.wait(lock, [&] { return !items.empty() && (items.front().pending == nullptr || items.front().pending->ready); });
            committerWaiting = false;
            commit = items.front().pending != nullptr ? &items.front().pending->commit : &items.front().commit;
        }
        const auto start = std::chrono::steady_clock::now();
        std::exception_ptr fatal;
        bool failed = false;
        try {
            if (*commit) (*commit)();
        } catch (const std::exception& error) {
            failed = true;
            static std::atomic<std::uint64_t> reported{0};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 20) std::fprintf(stderr, "[draw] pipelined commit failed: %.200s\n", error.what());
        } catch (...) {
            fatal = std::current_exception();
        }
        const auto spent = elapsedNs(start);
        std::lock_guard lock(mutex);
        items.pop_front();
        ++commits;
        commitNs += spent;
        if (failed) ++commitErrors;
        if (fatal != nullptr && failure == nullptr) failure = fatal;
        outstanding.fetch_sub(1, std::memory_order_release);
        if (idleWaiters != 0) idle.notify_all();
    }
}

void DrawPipeline::Drain(DrainReason reason, std::uint32_t opcode) {
    if (!Busy()) return;
    if (GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw pipeline drain under the GPU lock (reason " + std::to_string(static_cast<unsigned>(reason)) + ", opcode " + std::to_string(opcode) + ")");
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock lock(mutex);
    ++idleWaiters;
    idle.wait(lock, [&] { return items.empty(); });
    --idleWaiters;
    const auto index = static_cast<std::size_t>(reason);
    ++drains[index];
    ++drainOpcodes[std::min<std::uint32_t>(opcode, 0x100)];
    drainWaitNs[index] += elapsedNs(start);
    rethrowFailure();
}

std::optional<std::uint64_t> DrawPipeline::PendingLabel(std::uint64_t address, std::size_t bytes) {
    if (bytes == 0 || bytes > 8) return std::nullopt;
    const auto end = address + bytes;
    std::lock_guard lock(mutex);
    for (auto item = items.rbegin(); item != items.rend(); ++item) {
        if (item->pending != nullptr && !item->pending->ready) return std::nullopt;
        const auto& writes = item->pending != nullptr ? item->pending->writes : item->writes;
        const bool overlaps = std::any_of(writes.begin(), writes.end(), [&](const Range& range) { return address < range.second && range.first < end; });
        if (!overlaps) continue;
        if (item->labelBytes.empty() || address < item->labelAddress || end > item->labelAddress + item->labelBytes.size()) return std::nullopt;
        std::uint64_t value = 0;
        std::memcpy(&value, item->labelBytes.data() + (address - item->labelAddress), bytes);
        return value;
    }
    return std::nullopt;
}

bool DrawPipeline::Overlaps(std::uint64_t address, std::size_t bytes, std::uint64_t before) {
    const auto end = address + bytes;
    std::unique_lock lock(mutex);
    for (;;) {
        bool waiting = false;
        for (const auto& item : items) {
            if (item.seq >= before) break;
            if (item.pending != nullptr && !item.pending->ready) {
                waiting = true;
                break;
            }
            const auto& writes = item.pending != nullptr ? item.pending->writes : item.writes;
            for (const auto& [begin, limit] : writes) {
                if (address < limit && begin < end) return true;
            }
        }
        if (!waiting) return false;
        // A helper holding the GPU lock must not wait for another helper, which may need the lock.
        if (GuestMemory::GpuMutex().HeldByThisThread()) return true;
        ++idleWaiters;
        idle.wait(lock);
        --idleWaiters;
    }
}

void DrawPipeline::DrainBefore(std::uint64_t seq) {
    if (GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw pipeline drain under the GPU lock (helper)");
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock lock(mutex);
    ++idleWaiters;
    idle.wait(lock, [&] { return items.empty() || items.front().seq >= seq; });
    --idleWaiters;
    ++drains[static_cast<std::size_t>(DrainReason::Capture)];
    drainWaitNs[static_cast<std::size_t>(DrainReason::Capture)] += elapsedNs(start);
}

}
