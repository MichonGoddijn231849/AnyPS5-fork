#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_CAPTURE_CAPTUREFORMAT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_CAPTURE_CAPTUREFORMAT_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace AgcDriver::Capture {

inline constexpr std::array<char, 8> Magic{'A', 'P', 'S', '5', 'C', 'A', 'P', '1'};
inline constexpr std::uint32_t Version = 2;
inline constexpr std::size_t PageBytes = 4096;
inline constexpr std::uint32_t ZeroPage = 0xffffffffu;
inline constexpr std::uint32_t QueueCount = 0x58;

enum class EventType : std::uint32_t {
    Begin = 1,
    AddressSpace = 2,
    Memory = 3,
    Progress = 4,
    Submit = 5,
    Suspend = 6,
    Shader = 7,
    QueueState = 8,
    DriverState = 9,
    VideoOutput = 10,
    Present = 11,
    End = 12,
};

struct EventHeader {
    EventType type;
    std::uint32_t reserved;
    std::uint64_t bytes;
};

enum class PieceKind : std::uint8_t { Private = 0, Direct = 1, External = 2 };

struct Piece {
    std::uint64_t address;
    std::uint64_t bytes;
    PieceKind kind;
    std::uint8_t readable;
    std::uint8_t writable;
    std::uint8_t gpu;
    std::int32_t memoryType;
    std::uint32_t backing;
    std::uint32_t reserved2;
    std::uint64_t backingOffset;
    bool operator==(const Piece&) const = default;
};

struct Backing {
    std::uint32_t id;
    std::uint32_t reserved;
    std::uint64_t bytes;
};

struct RegistryRange {
    std::uint64_t address;
    std::uint64_t bytes;
    std::uint8_t readable;
    std::uint8_t writable;
    std::uint8_t gpu;
    std::uint8_t reserved;
    std::int32_t sceProtection;
    bool operator==(const RegistryRange&) const = default;
};

struct MemoryRun {
    std::uint64_t address;
    std::uint32_t pages;
    std::uint32_t reserved;
};

enum class MemoryKind : std::uint32_t { Base = 0, Delta = 1, Mapped = 2 };

struct BeginEvent {
    std::uint64_t firstFrame;
    std::uint64_t lastFrame;
    std::uint64_t flipsBefore;
    std::uint64_t reserved;
};

struct SubmitEvent {
    std::uint32_t queue;
    std::uint32_t flips;
    std::uint64_t packetAddress;
    std::uint32_t packetWords;
    std::uint32_t words;
    std::uint64_t hash;
};

struct DisplayBufferRecord {
    std::uint64_t address;
    std::uint64_t pixelFormat;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t tilingMode;
    std::uint32_t pitchInPixel;
};

struct PresentEvent {
    std::uint64_t flip;
    std::uint8_t hasBuffer;
    std::uint8_t opaque;
    std::uint8_t reserved[6];
    DisplayBufferRecord buffer;
};

struct VideoOutputEvent {
    std::uint32_t handle;
    std::uint32_t registered;
};

struct EndEvent {
    std::uint64_t flips;
    std::uint64_t submissions;
};

inline std::uint64_t HashWords(std::span<const std::uint32_t> words) {
    std::uint64_t hash = 0xcbf29ce484222325ull ^ words.size();
    for (const auto word : words) {
        hash ^= word;
        hash *= 0x100000001b3ull;
        hash ^= hash >> 29u;
    }
    return hash;
}

class Writer {
public:
    template<typename T>
    void Put(const T& value) {
        const auto* bytes = reinterpret_cast<const std::byte*>(&value);
        data.insert(data.end(), bytes, bytes + sizeof(T));
    }
    template<typename T>
    void PutSpan(std::span<const T> values) {
        Put<std::uint64_t>(values.size());
        const auto* bytes = reinterpret_cast<const std::byte*>(values.data());
        data.insert(data.end(), bytes, bytes + values.size_bytes());
    }
    void PutString(const std::string& text) {
        PutSpan(std::span<const char>(text));
    }
    std::vector<std::byte> data;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> data) : data(data) {}
    template<typename T>
    T Get() {
        if (data.size() - cursor < sizeof(T)) throw std::runtime_error("capture event is truncated");
        T value;
        std::memcpy(&value, data.data() + cursor, sizeof(T));
        cursor += sizeof(T);
        return value;
    }
    template<typename T>
    std::span<const T> GetSpan() {
        const auto count = Get<std::uint64_t>();
        if (count > (data.size() - cursor) / sizeof(T)) throw std::runtime_error("capture event array is truncated");
        const std::span<const T> values(reinterpret_cast<const T*>(data.data() + cursor), static_cast<std::size_t>(count));
        cursor += static_cast<std::size_t>(count) * sizeof(T);
        return values;
    }
    std::string GetString() {
        const auto text = GetSpan<char>();
        return {text.begin(), text.end()};
    }
    bool Done() const { return cursor == data.size(); }

private:
    std::span<const std::byte> data;
    std::size_t cursor = 0;
};

}

#endif
