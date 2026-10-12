#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_CAPTURE_PASSCAPTURE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_CAPTURE_PASSCAPTURE_HPP

#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

struct ShaderSnapshot;

// APS5_CAPTURE_PASSES=<file> (tools/fidelity, docs/dev/Fidelity.md): the passes of one replay whose inputs, packets and
// outputs are saved, one line each:
//   <program hex> <instance> <directory> [<address hex>:<bytes hex>:<i|o>,...]   a dispatch: the program's n-th
//                                                                                 [dispatch-io] header from 0
//   draws <first serial> <last serial> <directory>                               a render pass: the draws whose
//                                                                                 [draw-io] serials lie in the range
// A pass directory receives packets/<n>.state and <n>.pm4 (the queue state at each packet and its words), shaders/
// (the registered programs the packets use), before/<address>.bin (each range the pass reads, as it was the first time
// the pass touched it, GPU results flushed), after/<address>.bin (the ranges it writes, after its last packet),
// ranges.txt ("<address> <bytes> <i|o>") and done.txt. agc_pass_run runs it alone. Equal range contents are saved once
// under <capture file>.blobs/ and hard-linked; APS5_CAPTURE_PASSES_BUDGET_MIB stops new saves past that many MiB.
class PassCapture {
public:
    using Range = std::pair<std::uint64_t, std::uint64_t>;
    struct Pass {
        bool draws = false;
        std::uint64_t program = 0;
        std::uint64_t first = 0;
        std::uint64_t last = 0;
        std::filesystem::path directory;
        // Dispatch passes: the ranges the inventory listed (outputs flagged).
        std::vector<std::pair<Range, bool>> listed;
        std::mutex mutex;
        std::set<std::uint64_t> savedBefore;
        std::vector<Range> outputs;
        std::uint32_t packets = 0;
        bool done = false;
        bool overBudget = false;
        // What the driver checked (GuestMemory::CheckRange) while working on the pass (RecordScope): reads the
        // bindings do not list (vertex inputs, decode reads); saved with the next SaveBefore or at Finish.
        std::vector<std::pair<Range, bool>> recorded;
    };

    // Records the guest ranges this thread checks while it works on `pass` (no-op for null).
    class RecordScope {
    public:
        explicit RecordScope(Pass* pass);
        ~RecordScope();
        RecordScope(const RecordScope&) = delete;
        RecordScope& operator=(const RecordScope&) = delete;

    private:
        Pass* pass;
        std::vector<GuestMemory::CheckedRange>* previous = nullptr;
        std::vector<GuestMemory::CheckedRange> ranges;
    };

    static bool Active();
    static Pass* ForDispatch(std::uint64_t program, std::uint64_t instance);
    static Pass* ForDraw(std::uint64_t serial);

    // The packet with the queue state it runs in, and the programs it uses.
    static void SavePacket(Pass& pass, const QueueState& queue, std::uint32_t queueId, std::span<const std::uint32_t> packet, std::span<const std::shared_ptr<const ShaderSnapshot>> shaders);
    // Saves each range the pass has not saved yet (call with GPU results flushed); outputs are remembered for Finish.
    // `captured`: ranges whose content the driver took at the packet (saved from those bytes, not live memory).
    static void SaveBefore(Pass& pass, std::span<const Range> reads, std::span<const Range> writes, std::span<const std::pair<std::uint64_t, std::span<const std::byte>>> captured = {});
    // Saves the remembered outputs (call with GPU results flushed) and marks the pass done.
    static void Finish(Pass& pass);
};

}

#endif
