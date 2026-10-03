#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_CAPTURE_FRAMECAPTURE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_CAPTURE_FRAMECAPTURE_HPP

#include "prx/libSceAgcDriver/Execution/include/Driver/Capture/CaptureFormat.hpp"
#include "prx/libSceAgcDriver/Execution/include/DisplayBuffer.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace AgcDriver::Capture {

class CaptureFailed : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct AddressSpace {
    std::vector<Piece> pieces;
    std::vector<RegistryRange> registry;
};

struct Hash128 {
    std::uint64_t low;
    std::uint64_t high;
    bool operator==(const Hash128&) const = default;
};

struct Hash128Hasher {
    std::size_t operator()(const Hash128& hash) const { return static_cast<std::size_t>(hash.low ^ (hash.high * 0x9e3779b97f4a7c15ull)); }
};

class FrameCapture {
public:
    enum class Phase : std::uint8_t { Off, Waiting, Recording, Done };

    static FrameCapture& Get();

    bool Active() const {
        const auto current = phase.load(std::memory_order_acquire);
        return current == Phase::Waiting || current == Phase::Recording;
    }
    bool Recording() const { return phase.load(std::memory_order_acquire) == Phase::Recording; }
    std::mutex& SubmitMutex() { return submitMutex; }
    std::uint64_t FirstFrame() const { return firstFrame; }
    std::uint64_t LastFrame() const { return lastFrame; }

    std::uint64_t flips = 0;
    std::uint64_t nextAttempt = 0;
    std::uint64_t submissions = 0;

    void Begin(std::uint64_t flipsBefore);
    void RecordEvent(EventType type, std::span<const std::byte> payload);
    void RecordAddressSpaceChanges();
    void RecordDelta(std::span<const std::uint64_t> progress);
    void RecordShader(std::uint64_t header, std::span<const std::pair<std::uint64_t, std::uint64_t>> blocks);
    void RecordSubmit(const SubmitEvent& submit, std::span<const std::uint32_t> words);
    void NotePresent(const DisplayBuffer* buffer, bool opaque, std::string& dumpPath);
    bool PresentsReached(std::uint64_t flipsEnd) const;
    void Finish(const EndEvent& end);
    void Fail(const std::string& reason);
    std::uint64_t FlipsBefore() const { return flipsBefore; }

private:
    FrameCapture();
    std::uint32_t storePage(const std::byte* page);
    void recordRanges(MemoryKind kind, std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, bool flush);
    void recordPages(MemoryKind kind, std::span<const std::uint64_t> pages);
    AddressSpace snapshotAddressSpace(std::vector<Backing>& added);
    void writeSummary(const char* status, const std::string& detail);
    void checkLimit();
    void closeFiles();

    std::atomic<Phase> phase{Phase::Off};
    std::mutex submitMutex;
    std::mutex writerMutex;
    std::string directory;
    std::uint64_t firstFrame = 0;
    std::uint64_t lastFrame = 0;
    std::uint64_t limitBytes = 0;
    std::FILE* events = nullptr;
    std::FILE* pages = nullptr;
    std::uint64_t eventBytes = 0;
    std::uint32_t storedPages = 0;
    std::unordered_map<Hash128, std::uint32_t, Hash128Hasher> pageIndex;
    std::map<const void*, std::uint32_t> backingIds;
    AddressSpace current;
    std::uint64_t registryGeneration = 0;
    std::uint64_t flipsBefore = 0;
    std::atomic<std::uint64_t> presentsSeen{0};

    struct Counts {
        std::uint64_t pages = 0;
        std::uint64_t zeroPages = 0;
        std::uint64_t storedPages = 0;
    };
    Counts base;
    Counts mapped;
    Counts shader;
    std::vector<Counts> deltaPerFrame;
    std::uint64_t addressSpaceChanges = 0;
    std::uint64_t piecesAdded = 0;
    std::uint64_t piecesRemoved = 0;
    std::uint64_t deltaPoints = 0;
    std::uint64_t submitWords = 0;
    std::map<std::uint32_t, std::uint64_t> submitsPerQueue;
    std::uint64_t shaders = 0;
    double baseSeconds = 0;
    double collectSeconds = 0;
    std::chrono::steady_clock::time_point startedAt{};
};

}

#endif
