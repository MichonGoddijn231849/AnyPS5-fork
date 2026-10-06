#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWPIPELINE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWPIPELINE_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

class DrawPipeline {
public:
    using Commit = std::function<void()>;
    using Range = std::pair<std::uint64_t, std::uint64_t>;
    enum class DrainReason : std::uint8_t { Packet, Flush, Labels, Capture, Indirect, Submission, Count };

    static DrawPipeline& Queue0();
    static std::size_t Depth();
    static bool& Active();
    static std::atomic<std::uint64_t>& EpochToken();
    static void FollowEpoch(std::uint64_t token);

    // A draw whose preparation runs on a helper thread (APS5_PARALLEL_DRAWS): its place in the
    // order is taken at the packet, its commit and writes are published when the helper finishes.
    // The committer waits for it in order; a helper's capture waits only for earlier items.
    struct PendingDraw {
        bool ready = false;
        Commit commit;
        std::vector<Range> writes;
    };
    static std::size_t Helpers();
    // The sequence number of the item the calling helper prepares (0 on the queue worker).
    static std::uint64_t& CurrentSeq();
    // Runs `job` on the helper pool, in submission order (FIFO).
    void RunOnHelper(std::function<void()> job);

    void Enqueue(Commit commit, std::vector<Range> writes, std::uint64_t labelAddress = 0, std::vector<std::byte> labelBytes = {});
    std::uint64_t EnqueuePending(std::shared_ptr<PendingDraw> pending);
    void Publish(const std::shared_ptr<PendingDraw>& pending, Commit commit, std::vector<Range> writes);
    void Drain(DrainReason reason, std::uint32_t opcode = 0x100);
    // Waits until every item before `seq` is committed.
    void DrainBefore(std::uint64_t seq);
    bool Busy() const { return outstanding.load(std::memory_order_acquire) != 0; }
    // Whether an in-flight item before `before` writes the range; a pending draw among them is waited
    // for until its writes are known.
    bool Overlaps(std::uint64_t address, std::size_t bytes, std::uint64_t before = ~std::uint64_t{0});
    // The value the newest in-flight write of [address, address + bytes) stores, when that write is
    // a label covering the whole range (bytes <= 8, little endian).
    std::optional<std::uint64_t> PendingLabel(std::uint64_t address, std::size_t bytes);

private:
    struct Item {
        Commit commit;
        std::vector<Range> writes;
        std::uint64_t labelAddress = 0;
        std::vector<std::byte> labelBytes;
        std::uint64_t seq = 0;
        std::shared_ptr<PendingDraw> pending;
    };
    DrawPipeline() = default;
    void run();
    void helperLoop();
    void push(Item item, std::unique_lock<std::mutex>& lock);
    void rethrowFailure();
    void report(std::chrono::steady_clock::time_point now);

    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::deque<Item> items;
    std::exception_ptr failure;
    std::atomic<std::size_t> outstanding{0};
    std::uint32_t idleWaiters = 0;
    bool committerWaiting = false;
    std::uint64_t nextSeq = 1;
    std::thread thread;
    std::mutex helperMutex;
    std::condition_variable helperWake;
    std::deque<std::function<void()>> helperJobs;
    std::vector<std::thread> helpers;
    std::uint64_t commits = 0;
    std::uint64_t commitNs = 0;
    std::uint64_t commitErrors = 0;
    std::uint64_t enqueued = 0;
    std::uint64_t fullWaitNs = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(DrainReason::Count)> drains{};
    std::array<std::uint64_t, static_cast<std::size_t>(DrainReason::Count)> drainWaitNs{};
    std::array<std::uint64_t, 257> drainOpcodes{};
    std::uint64_t depthSum = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
};

}

#endif
