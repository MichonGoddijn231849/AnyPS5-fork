#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Capture/CaptureFormat.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Capture/Replay.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_vulkan.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace {

using namespace AgcDriver::Capture;
using Clock = std::chrono::steady_clock;

constexpr std::uint64_t ViewBytes = 0x4000;
constexpr std::uint64_t ReserveGranule = 0x10000;

[[noreturn]] void Fail(const std::string& reason) {
    throw std::runtime_error(reason);
}

std::string Hex(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

struct Options {
    std::filesystem::path capture;
    std::uint32_t loops = 1;
    double forSeconds = 0;
    std::optional<std::filesystem::path> png;
    std::uint32_t pngScale = 1;
    bool pngAllLoops = false;
    bool deltaMerge = true;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> dumpRanges;
    std::optional<std::filesystem::path> compare;
    bool pacing = true;
    bool settle = false;
    bool hidden = false;
    // Collect the restore's writes before the loop starts (see restore()).
    bool restoreCollect = true;
    std::optional<std::filesystem::path> shaderCache;
    std::uint32_t cold = 0;
};

struct Event {
    EventType type;
    std::span<const std::byte> payload;
};

class CaptureFile {
public:
    explicit CaptureFile(const std::filesystem::path& directory) {
        std::ifstream input(directory / "events.bin", std::ios::binary);
        if (!input) Fail("cannot open " + (directory / "events.bin").string());
        input.seekg(0, std::ios::end);
        bytes.resize(static_cast<std::size_t>(input.tellg()));
        input.seekg(0);
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input) Fail("cannot read events.bin");
        constexpr std::size_t headerBytes = Magic.size() + 8;
        if (bytes.size() < headerBytes || std::memcmp(bytes.data(), Magic.data(), Magic.size()) != 0) Fail("events.bin is not an AnyPS5 capture");
        std::uint32_t version;
        std::memcpy(&version, bytes.data() + Magic.size(), 4);
        if (version != Version) Fail("capture format version " + std::to_string(version) + " is not supported");
        for (std::size_t cursor = headerBytes; cursor < bytes.size();) {
            if (bytes.size() - cursor < sizeof(EventHeader)) Fail("events.bin ends inside an event header (the capture did not finish)");
            EventHeader header;
            std::memcpy(&header, bytes.data() + cursor, sizeof(header));
            cursor += sizeof(header);
            if (header.bytes > bytes.size() - cursor) Fail("events.bin ends inside an event (the capture did not finish)");
            events.push_back({header.type, std::span(bytes).subspan(cursor, static_cast<std::size_t>(header.bytes))});
            cursor += static_cast<std::size_t>(header.bytes);
        }
        if (events.empty() || events.back().type != EventType::End) Fail("the capture has no End event: it failed or did not finish (see capture.txt)");
        const auto pagesPath = (directory / "pages.bin").wstring();
        file = CreateFileW(pagesPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) Fail("cannot open pages.bin");
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size)) Fail("cannot size pages.bin");
        pageCount = static_cast<std::uint64_t>(size.QuadPart) / PageBytes;
        if (pageCount != 0) {
            mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (mapping == nullptr) Fail("cannot map pages.bin");
            pages = static_cast<const std::byte*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
            if (pages == nullptr) Fail("cannot map a view of pages.bin");
        }
    }

    ~CaptureFile() {
        if (pages != nullptr) UnmapViewOfFile(pages);
        if (mapping != nullptr) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }

    CaptureFile(const CaptureFile&) = delete;
    CaptureFile& operator=(const CaptureFile&) = delete;

    const std::byte* Page(std::uint32_t index) const {
        if (index == ZeroPage) return nullptr;
        if (index >= pageCount) Fail("capture references page " + std::to_string(index) + " beyond pages.bin");
        return pages + static_cast<std::uint64_t>(index) * PageBytes;
    }

    std::vector<Event> events;

private:
    std::vector<std::byte> bytes;
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
    const std::byte* pages = nullptr;
    std::uint64_t pageCount = 0;
};

DWORD Protection(const Piece& piece) {
    if (piece.readable && piece.writable) return PAGE_READWRITE;
    if (piece.readable) return PAGE_READONLY;
    return PAGE_NOACCESS;
}

bool PieceLess(const Piece& a, const Piece& b) {
    return std::tie(a.address, a.bytes, a.kind, a.backing, a.backingOffset, a.readable, a.writable, a.gpu, a.memoryType) < std::tie(b.address, b.bytes, b.kind, b.backing, b.backingOffset, b.readable, b.writable, b.gpu, b.memoryType);
}

bool RegistryLess(const RegistryRange& a, const RegistryRange& b) {
    return std::tie(a.address, a.bytes, a.readable, a.writable, a.gpu, a.sceProtection) < std::tie(b.address, b.bytes, b.readable, b.writable, b.gpu, b.sceProtection);
}

class GuestSpace {
public:
    ~GuestSpace() {
        for (const auto& [id, section] : sections) CloseHandle(section);
    }

    void Apply(std::span<const Backing> backings, std::span<const Piece> removed, std::span<const Piece> added, std::span<const RegistryRange> registryRemoved, std::span<const RegistryRange> registryAdded) {
        for (const auto& backing : backings) {
            if (sections.contains(backing.id)) continue;
            const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, static_cast<DWORD>(backing.bytes >> 32u), static_cast<DWORD>(backing.bytes), nullptr);
            if (section == nullptr) Fail("cannot create a direct memory section of " + Hex(backing.bytes) + " bytes");
            sections.emplace(backing.id, section);
        }
        {
            GuestAllocations::Mutation mutation;
            for (const auto& range : registryRemoved) mutation.Remove(reinterpret_cast<const void*>(range.address));
        }
        for (const auto& piece : removed) unmap(piece);
        std::vector<Piece> external;
        for (const auto& piece : added) {
            if (piece.kind == PieceKind::External) external.push_back(piece);
        }
        reserveExternal(external);
        for (const auto& piece : added) {
            map(piece);
            if (Protection(piece) != PAGE_READWRITE) pendingProtections.push_back(piece);
        }
        {
            GuestAllocations::Mutation mutation;
            std::uintptr_t arenaBase = 0;
            std::size_t arenaBytes = 0;
            GuestArena::GuestArenaRange_nid_postfix(&arenaBase, &arenaBytes);
            for (const auto& range : registryAdded) {
                const bool image = range.address < arenaBase || range.address + range.bytes > arenaBase + arenaBytes;
                if (image) mutation.AddImage(reinterpret_cast<void*>(range.address), static_cast<std::size_t>(range.bytes), range.readable != 0, range.writable != 0);
                else mutation.Add(reinterpret_cast<void*>(range.address), static_cast<std::size_t>(range.bytes), range.readable != 0, range.writable != 0);
            }
        }
        for (const auto& piece : removed) erase(pieces, piece, PieceLess);
        for (const auto& piece : added) insert(pieces, piece, PieceLess);
        for (const auto& range : registryRemoved) erase(registry, range, RegistryLess);
        for (const auto& range : registryAdded) insert(registry, range, RegistryLess);
    }

    void ApplyProtections() {
        for (const auto& piece : pendingProtections) protect(piece, Protection(piece));
        pendingProtections.clear();
    }

    // A merge writes only the words the capture changed since `previous` (the page's earlier captured
    // version, null for zeros): the others may hold what the replay's GPU wrote ahead of the capture
    // (labels, buffers), which the capture saw only later or never.
    void Write(std::uint64_t address, const std::byte* source, std::size_t bytes, bool compareFirst, bool merge = false, const std::byte* previous = nullptr) {
        auto* target = reinterpret_cast<std::byte*>(address);
        if (compareFirst) {
            const bool same = source != nullptr ? std::memcmp(target, source, bytes) == 0 : std::all_of(target, target + bytes, [](std::byte value) { return value == std::byte{0}; });
            if (same) return;
            ++restoredPages;
            restoredRanges.emplace_back(address & ~std::uint64_t{4095}, (address + bytes + 4095) & ~std::uint64_t{4095});
        }
        const auto* piece = find(address);
        if (piece == nullptr) Fail("capture writes " + Hex(address) + " outside every mapped piece");
        const bool applied = std::find(pendingProtections.begin(), pendingProtections.end(), *piece) == pendingProtections.end();
        const auto store = [&] {
            if (merge) mergeWords(target, source, previous, bytes);
            else copy(target, source, bytes);
        };
        if (!applied || piece->writable) {
            store();
            return;
        }
        const auto page = address & ~(ViewBytes - 1);
        DWORD protection;
        if (!VirtualProtect(reinterpret_cast<void*>(page), ViewBytes, PAGE_READWRITE, &protection)) Fail("cannot open read-only guest page " + Hex(page) + " for a capture write");
        store();
        if (!VirtualProtect(reinterpret_cast<void*>(page), ViewBytes, protection, &protection)) Fail("cannot restore guest page protection at " + Hex(page));
    }

    const std::vector<Piece>& Pieces() const { return pieces; }
    const std::vector<RegistryRange>& Registry() const { return registry; }
    std::uint64_t restoredPages = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> restoredRanges;

private:
    template<typename T, typename TLess>
    static void insert(std::vector<T>& values, const T& value, TLess less) {
        values.insert(std::upper_bound(values.begin(), values.end(), value, less), value);
    }

    template<typename T, typename TLess>
    static void erase(std::vector<T>& values, const T& value, TLess less) {
        const auto found = std::lower_bound(values.begin(), values.end(), value, less);
        if (found == values.end() || less(value, *found) || less(*found, value)) Fail("capture removes a range the replay never mapped");
        values.erase(found);
    }

    static void copy(std::byte* target, const std::byte* source, std::size_t bytes) {
        if (source != nullptr) std::memcpy(target, source, bytes);
        else std::memset(target, 0, bytes);
    }

    static void mergeWords(std::byte* target, const std::byte* source, const std::byte* previous, std::size_t bytes) {
        for (std::size_t offset = 0; offset < bytes; offset += sizeof(std::uint32_t)) {
            std::uint32_t now = 0;
            std::uint32_t before = 0;
            if (source != nullptr) std::memcpy(&now, source + offset, sizeof(now));
            if (previous != nullptr) std::memcpy(&before, previous + offset, sizeof(before));
            if (now != before) std::memcpy(target + offset, &now, sizeof(now));
        }
    }

    const Piece* find(std::uint64_t address) const {
        auto it = std::upper_bound(pieces.begin(), pieces.end(), address, [](std::uint64_t value, const Piece& piece) { return value < piece.address; });
        if (it == pieces.begin()) return nullptr;
        --it;
        return address < it->address + it->bytes ? &*it : nullptr;
    }

    void reserveExternal(const std::vector<Piece>& external) {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> spans;
        for (const auto& piece : external) spans.emplace_back(piece.address & ~(ReserveGranule - 1), (piece.address + piece.bytes + ReserveGranule - 1) & ~(ReserveGranule - 1));
        std::sort(spans.begin(), spans.end());
        std::vector<std::pair<std::uint64_t, std::uint64_t>> merged;
        for (const auto& span : spans) {
            if (!merged.empty() && span.first <= merged.back().second) merged.back().second = std::max(merged.back().second, span.second);
            else merged.push_back(span);
        }
        for (const auto& [begin, end] : merged) {
            for (auto cursor = begin; cursor < end;) {
                MEMORY_BASIC_INFORMATION memory{};
                if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) != sizeof(memory)) Fail("cannot query the replay address space at " + Hex(cursor));
                const auto stop = std::min<std::uint64_t>(end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
                if (memory.State == MEM_FREE) {
                    if (VirtualAlloc(reinterpret_cast<void*>(cursor), static_cast<SIZE_T>(stop - cursor), MEM_RESERVE, PAGE_NOACCESS) == nullptr) Fail("cannot reserve the captured image range " + Hex(cursor) + "-" + Hex(stop) + " (the replay process already uses it)");
                } else if (memory.Type != MEM_PRIVATE || externalReserved.find(reinterpret_cast<std::uintptr_t>(memory.AllocationBase)) == externalReserved.end()) {
                    Fail("captured image range " + Hex(cursor) + "-" + Hex(stop) + " collides with memory of the replay process (a DLL?)");
                }
                externalReserved.insert(reinterpret_cast<std::uintptr_t>(memory.State == MEM_FREE ? reinterpret_cast<void*>(cursor) : memory.AllocationBase));
                cursor = stop;
            }
        }
    }

    void map(const Piece& piece) {
        auto* pointer = reinterpret_cast<void*>(piece.address);
        const auto bytes = static_cast<std::size_t>(piece.bytes);
        if (piece.kind == PieceKind::External) {
            if (VirtualAlloc(pointer, bytes, MEM_COMMIT, PAGE_READWRITE) == nullptr) Fail("cannot commit the captured image range " + Hex(piece.address));
            return;
        }
        GuestArena::GuestArenaMarkUsed_nid_postfix(pointer, bytes);
        if (piece.kind == PieceKind::Direct) {
            const auto section = sections.find(piece.backing);
            if (section == sections.end()) Fail("capture maps an unknown direct memory backing");
            GuestArena::GuestArenaMap_nid_postfix(pointer, bytes, section->second, piece.backingOffset, PAGE_READWRITE);
        } else if (piece.readable || piece.writable) {
            GuestArena::GuestArenaCommit_nid_postfix(pointer, bytes, PAGE_READWRITE, ViewBytes);
        }
    }

    void unmap(const Piece& piece) {
        auto* pointer = reinterpret_cast<void*>(piece.address);
        const auto bytes = static_cast<std::size_t>(piece.bytes);
        std::erase(pendingProtections, piece);
        if (piece.kind == PieceKind::External) {
            if (!VirtualFree(pointer, bytes, MEM_DECOMMIT)) Fail("cannot decommit the captured image range " + Hex(piece.address));
            return;
        }
        GuestArena::GuestArenaReset_nid_postfix(pointer, bytes);
        GuestArena::GuestArenaRelease_nid_postfix(pointer, bytes);
    }

    void protect(const Piece& piece, DWORD protection) {
        if (piece.kind == PieceKind::Private && !piece.readable && !piece.writable) return;
        const auto step = piece.kind == PieceKind::External ? piece.bytes : ViewBytes;
        for (auto cursor = piece.address; cursor < piece.address + piece.bytes; cursor += step) {
            DWORD previous;
            const auto bytes = std::min<std::uint64_t>(step, piece.address + piece.bytes - cursor);
            if (!VirtualProtect(reinterpret_cast<void*>(cursor), static_cast<SIZE_T>(bytes), protection, &previous)) Fail("cannot protect guest memory at " + Hex(cursor));
        }
        if (piece.kind == PieceKind::Direct) GuestArena::GuestArenaSetProtection_nid_postfix(piece.address, static_cast<std::size_t>(piece.bytes), protection);
    }

    std::map<std::uint32_t, HANDLE> sections;
    std::vector<Piece> pieces;
    std::vector<RegistryRange> registry;
    std::vector<Piece> pendingProtections;
    std::set<std::uintptr_t> externalReserved;
};

struct PresentJob {
    bool clear = false;
    std::optional<PresentEvent> present;
    std::string dump;
    std::shared_ptr<AgcDriver::FrameTiming> timing;
};

class Presenter {
public:
    Presenter(std::uint32_t width, std::uint32_t height, bool hidden) : width(width), height(height), hidden(hidden) {
        thread = std::thread([this] { run(); });
    }

    ~Presenter() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        changed.notify_all();
        thread.join();
    }

    void Enqueue(PresentJob job) {
        {
            std::lock_guard lock(mutex);
            if (failure) std::rethrow_exception(failure);
            jobs.push_back(std::move(job));
            ++queued;
        }
        changed.notify_all();
    }

    void WaitDone() {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return failure != nullptr || done == queued; });
        if (failure) std::rethrow_exception(failure);
    }

    std::uint64_t Presented() {
        std::lock_guard lock(mutex);
        return done;
    }

private:
    void run() {
        try {
            if (SDL_Init(SDL_INIT_VIDEO) != 0) Fail(std::string("SDL_Init failed: ") + SDL_GetError());
            int x = SDL_WINDOWPOS_CENTERED;
            int y = SDL_WINDOWPOS_CENTERED;
            for (int display = 0; display < SDL_GetNumVideoDisplays(); ++display) {
                SDL_Rect bounds{};
                if (SDL_GetDisplayBounds(display, &bounds) == 0 && bounds.x < 0) {
                    x = bounds.x + 40;
                    y = bounds.y + 40;
                }
            }
            window = SDL_CreateWindow("agc_frame_replay", x, y, 1280, 720, SDL_WINDOW_VULKAN | (hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN));
            if (window == nullptr) Fail(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
            unsigned count = 0;
            if (!SDL_Vulkan_GetInstanceExtensions(window, &count, nullptr)) Fail(std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError());
            extensions.resize(count);
            if (!SDL_Vulkan_GetInstanceExtensions(window, &count, extensions.data())) Fail(std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError());
            for (;;) {
                PresentJob job;
                {
                    std::unique_lock lock(mutex);
                    while (!stopping && jobs.empty()) {
                        lock.unlock();
                        SDL_PumpEvents();
                        lock.lock();
                        changed.wait_for(lock, std::chrono::milliseconds(16), [&] { return stopping || !jobs.empty(); });
                    }
                    if (jobs.empty()) break;
                    job = std::move(jobs.front());
                    jobs.pop_front();
                }
                SDL_PumpEvents();
                present(job);
                {
                    std::lock_guard lock(mutex);
                    ++done;
                }
                changed.notify_all();
            }
            SDL_DestroyWindow(window);
        } catch (...) {
            {
                std::lock_guard lock(mutex);
                failure = std::current_exception();
            }
            changed.notify_all();
        }
    }

    void present(const PresentJob& job) {
        const bool buffer = job.present && job.present->hasBuffer;
        const AgcDriver::PresentationWindow target{window, extensions, [](void* context, VkInstance instance) {
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(context), instance, &surface)) Fail(std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError());
            return surface;
        }, [](void* context, std::uint32_t* drawableWidth, std::uint32_t* drawableHeight) {
            int w = 0;
            int h = 0;
            SDL_Vulkan_GetDrawableSize(static_cast<SDL_Window*>(context), &w, &h);
            *drawableWidth = w > 0 ? static_cast<std::uint32_t>(w) : 0;
            *drawableHeight = h > 0 ? static_cast<std::uint32_t>(h) : 0;
        }, buffer ? job.present->buffer.width : width, buffer ? job.present->buffer.height : height, job.timing};
        std::atomic<bool> ready{false};
        const auto gpuReady = [](void* context) { static_cast<std::atomic<bool>*>(context)->store(true); };
        if (buffer) {
            const auto& record = job.present->buffer;
            const AgcDriver::DisplayBuffer display{record.address, record.pixelFormat, record.width, record.height, record.tilingMode, record.pitchInPixel};
            if (!job.dump.empty()) ReplayDumpNextPresent(job.dump, dumpScale);
            AgcDriverPresentBuffer_nid_postfix(target, display, gpuReady, &ready);
        } else {
            AgcDriverPresentClear_nid_postfix(target, job.clear || !job.present || job.present->opaque, gpuReady, &ready);
        }
    }

public:
    std::uint32_t dumpScale = 1;

private:
    std::uint32_t width;
    std::uint32_t height;
    bool hidden;
    SDL_Window* window = nullptr;
    std::vector<const char*> extensions;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<PresentJob> jobs;
    std::uint64_t queued = 0;
    std::uint64_t done = 0;
    bool stopping = false;
    std::exception_ptr failure;
};

// APS5_TRACE_PACING=ms: the pacing waits longer than that, with every queue's progress, and the events that
// touch what queue 0 waits on.
double TracePacingMs() {
    static const double traceMs = [] {
        const char* value = std::getenv("APS5_TRACE_PACING");
        return value != nullptr ? std::strtod(value, nullptr) : -1.0;
    }();
    return traceMs;
}

class Replayer;

// Helper threads for large memory events: Run hands out `parts` indices to them and to the caller,
// and returns once every part ran (rethrowing the first part's failure).
class WritePool {
public:
    explicit WritePool(std::size_t threads) {
        for (std::size_t i = 0; i < threads; ++i) workers.emplace_back([this] { work(); });
    }
    WritePool(const WritePool&) = delete;
    WritePool& operator=(const WritePool&) = delete;
    ~WritePool() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        for (auto& worker : workers) worker.join();
    }

    void Run(std::size_t parts, const std::function<void(std::size_t)>& job) {
        {
            std::lock_guard lock(mutex);
            current = &job;
            total = parts;
            next = 0;
            finished = 0;
            failure = nullptr;
            ++generation;
        }
        wake.notify_all();
        claim();
        std::unique_lock lock(mutex);
        done.wait(lock, [&] { return finished == total; });
        current = nullptr;
        if (failure) std::rethrow_exception(failure);
    }

private:
    // Runs parts until none is left (the caller and the workers alike).
    void claim() {
        for (;;) {
            std::size_t part = 0;
            const std::function<void(std::size_t)>* job = nullptr;
            {
                std::lock_guard lock(mutex);
                if (current == nullptr || next >= total) return;
                part = next++;
                job = current;
            }
            std::exception_ptr error;
            try {
                (*job)(part);
            } catch (...) {
                error = std::current_exception();
            }
            std::lock_guard lock(mutex);
            if (error && !failure) failure = error;
            if (++finished == total) done.notify_all();
        }
    }

    void work() {
        std::uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stopping || (generation != seen && current != nullptr); });
                if (stopping) return;
                seen = generation;
            }
            claim();
        }
    }

    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable done;
    std::vector<std::thread> workers;
    const std::function<void(std::size_t)>* current = nullptr;
    std::size_t total = 0;
    std::size_t next = 0;
    std::size_t finished = 0;
    std::uint64_t generation = 0;
    std::exception_ptr failure;
    bool stopping = false;
};

class ReplayFlip final : public AgcDriver::IFlipRequest {
public:
    ReplayFlip(Replayer& owner, std::uint64_t flip) : owner(owner), flip(flip) {}
    void GpuReady(const std::shared_ptr<AgcDriver::FrameTiming>& timing) override;
    void Fail(std::exception_ptr error) noexcept override;

private:
    Replayer& owner;
    std::uint64_t flip;
};

class NoRenderingWait final : public AgcDriver::IRenderingWait {
public:
    void Wait() override {}
};

class ReplayOutput final : public AgcDriver::IVideoOutput {
public:
    explicit ReplayOutput(Replayer& owner) : owner(owner) {}
    std::shared_ptr<AgcDriver::IFlipRequest> Reserve(const AgcDriver::FlipInfo& info) override;
    std::shared_ptr<AgcDriver::IRenderingWait> CaptureRenderingWait(std::uint32_t) override { return std::make_shared<NoRenderingWait>(); }
    void Fail(std::exception_ptr error) noexcept override;

private:
    Replayer& owner;
};

struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
};

std::optional<Image> LoadPng(const std::filesystem::path& path, std::chrono::seconds wait) {
    const auto deadline = Clock::now() + wait;
    for (;;) {
        int width = 0;
        int height = 0;
        int channels = 0;
        if (auto* data = stbi_load(path.string().c_str(), &width, &height, &channels, 4)) {
            Image image{width, height, std::vector<std::uint8_t>(data, data + static_cast<std::size_t>(width) * height * 4)};
            stbi_image_free(data);
            return image;
        }
        if (Clock::now() >= deadline) return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

std::filesystem::path FramePath(const std::filesystem::path& directory, std::uint32_t loop, std::uint64_t flip) {
    char name[48];
    if (loop == 0) std::snprintf(name, sizeof(name), "frame_%05llu.png", static_cast<unsigned long long>(flip));
    else std::snprintf(name, sizeof(name), "loop%u_frame_%05llu.png", loop, static_cast<unsigned long long>(flip));
    return directory / name;
}

class Replayer {
public:
    Replayer(const Options& options, const CaptureFile& capture) : options(options), capture(capture) {
        for (std::size_t i = 0; i < capture.events.size(); ++i) {
            const auto& event = capture.events[i];
            if (event.type == EventType::Present) {
                Reader reader(event.payload);
                const auto present = reader.Get<PresentEvent>();
                if (presents.size() <= present.flip) presents.resize(static_cast<std::size_t>(present.flip) + 1);
                presents[static_cast<std::size_t>(present.flip)] = present;
            }
            if (prologueEnd == 0 && (event.type == EventType::Progress || event.type == EventType::Submit)) prologueEnd = i;
        }
        if (prologueEnd == 0) Fail("the capture has no submissions");
    }

    void Run() {
        std::uint32_t width = 1920;
        std::uint32_t height = 1080;
        for (const auto& present : presents) {
            if (present && present->hasBuffer) {
                width = present->buffer.width;
                height = present->buffer.height;
                break;
            }
        }
        if (options.png) std::filesystem::create_directories(*options.png);
        setImageRange();
        presenter = std::make_unique<Presenter>(width, height, options.hidden);
        presenter->dumpScale = options.pngScale;
        presenter->Enqueue({true, std::nullopt, {}, nullptr});
        presenter->WaitDone();
        std::optional<Clock::time_point> measuredSince;
        for (loop = 0; options.forSeconds > 0 ? (loop < 2 || std::chrono::duration<double>(Clock::now() - *measuredSince).count() < options.forSeconds) : loop < options.loops; ++loop) {
            if (loop == 1) measuredSince = Clock::now();
            const auto started = Clock::now();
            ReplayFeedRewindTails(true);
            if (loop == 0) {
                for (std::size_t i = 0; i < prologueEnd; ++i) apply(capture.events[i]);
                initialPieces = space.Pieces();
                initialRegistry = space.Registry();
                initialLastPage = lastPage;
            } else {
                restore();
            }
            dumpRanges("start");
            for (std::size_t queue = 0; queue < QueueCount; ++queue) base[queue] = ReplayPacketsExecuted(static_cast<std::uint32_t>(queue));
            nextFlip.store(0);
            const auto presentedBefore = presenter->Presented();
            const auto replayStarted = Clock::now();
            pacingMs = 0;
            memoryMs = 0;
            memoryBytes = 0;
            stallReleases = 0;
            submits = 0;
            mappingChanges = 0;
            piecesMapped = 0;
            piecesUnmapped = 0;
            for (std::size_t i = prologueEnd; i < capture.events.size(); ++i) apply(capture.events[i]);
            // Every tail of the capture is fed: a REWIND still waiting is for commands of the next frame.
            ReplayAbandonRewinds();
            const bool finished = drain();
            presenter->WaitDone();
            rethrowFailure();
            if (!options.dumpRanges.empty()) {
                ReplaySettle();
                dumpRanges("end");
            }
            const auto replayMs = std::chrono::duration<double, std::milli>(Clock::now() - replayStarted).count();
            const auto setupMs = std::chrono::duration<double, std::milli>(replayStarted - started).count();
            const auto frames = presenter->Presented() - presentedBefore;
            std::fprintf(stderr, "[replay] loop %u: %llu submissions, %llu frames in %.1f ms (%.2f FPS); setup %.1f ms; pacing waits %.1f ms, %llu stall releases; mapping changes %llu (+%llu/-%llu pieces); command mismatches %llu%s\n", loop, static_cast<unsigned long long>(submits), static_cast<unsigned long long>(frames), replayMs, frames != 0 ? 1000.0 * static_cast<double>(frames) / replayMs : 0.0, setupMs, pacingMs, static_cast<unsigned long long>(stallReleases), static_cast<unsigned long long>(mappingChanges), static_cast<unsigned long long>(piecesMapped), static_cast<unsigned long long>(piecesUnmapped), static_cast<unsigned long long>(ReplayCommandMismatches()), finished ? "" : "; QUEUES STILL BLOCKED at the end");
            {
                // Each frame's time: from the loop's start (frame 0) or the previous frame's GPU
                // completion to its own. Frame 0 carries the loop's restore effects (every page the
                // loop wrote is written back), later frames see only the capture's own changes.
                std::lock_guard lock(flipTimesMutex);
                std::string text;
                auto previous = replayStarted;
                for (const auto& at : flipTimes) {
                    char item[32];
                    std::snprintf(item, sizeof(item), " %.1f", std::chrono::duration<double, std::milli>(at - previous).count());
                    text += item;
                    previous = at;
                }
                std::fprintf(stderr, "[replay] loop %u: frame ms:%s\n", loop, text.c_str());
                std::fprintf(stderr, "[replay] loop %u: memory deltas written in %.1f ms (%.1f MiB)\n", loop, memoryMs, static_cast<double>(memoryBytes) / 1048576.0);
                flipTimes.clear();
            }
            if (!finished) {
                stuck = true;
                break;
            }
        }
        ReplaySettle();
        if (options.png) waitForFrames();
    }

    std::shared_ptr<AgcDriver::IFlipRequest> ReserveFlip() {
        return std::make_shared<ReplayFlip>(*this, nextFlip.fetch_add(1));
    }

    void FlipReady(std::uint64_t flip, const std::shared_ptr<AgcDriver::FrameTiming>& timing) {
        {
            std::lock_guard lock(flipTimesMutex);
            if (flipTimes.size() <= flip) flipTimes.resize(static_cast<std::size_t>(flip) + 1);
            flipTimes[static_cast<std::size_t>(flip)] = Clock::now();
        }
        PresentJob job;
        job.timing = timing;
        if (flip < presents.size()) job.present = presents[static_cast<std::size_t>(flip)];
        if (job.present && job.present->hasBuffer && options.png && (loop == 0 || options.pngAllLoops)) job.dump = FramePath(*options.png, loop, flip).string();
        presenter->Enqueue(std::move(job));
    }

    void NoteFailure(std::exception_ptr error) {
        std::lock_guard lock(failureMutex);
        if (!failure) failure = error;
    }

    bool Stuck() const { return stuck; }
    const std::vector<std::optional<PresentEvent>>& Presents() const { return presents; }

private:
    void waitForFrames() {
        const auto loops = options.pngAllLoops ? loop : std::min<std::uint32_t>(loop, 1);
        for (std::uint32_t written = 0; written < loops; ++written) {
            for (std::size_t flip = 0; flip < presents.size(); ++flip) {
                if (!presents[flip] || !presents[flip]->hasBuffer) continue;
                const auto path = FramePath(*options.png, written, flip);
                if (!LoadPng(path, std::chrono::seconds(30))) std::fprintf(stderr, "[replay] %s was not written\n", path.string().c_str());
            }
        }
    }

    void setImageRange() {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
        for (std::size_t i = 0; i < prologueEnd; ++i) {
            if (capture.events[i].type != EventType::AddressSpace) continue;
            Reader reader(capture.events[i].payload);
            static_cast<void>(reader.GetSpan<Backing>());
            static_cast<void>(reader.GetSpan<Piece>());
            for (const auto& piece : reader.GetSpan<Piece>()) {
                if (piece.kind != PieceKind::External) continue;
                if (!runs.empty() && piece.address >= runs.back().second && piece.address - runs.back().second < (16u << 20u)) runs.back().second = piece.address + piece.bytes;
                else runs.emplace_back(piece.address, piece.address + piece.bytes);
            }
            break;
        }
        if (runs.empty()) return;
        const auto largest = std::max_element(runs.begin(), runs.end(), [](const auto& a, const auto& b) { return a.second - a.first < b.second - b.first; });
        AgcDriver::GuestMemory::SetImageRange(static_cast<std::uintptr_t>(largest->first), static_cast<std::size_t>(largest->second - largest->first));
        std::fprintf(stderr, "[replay] guest image %s-%s (page-state cached as the game's main image)\n", Hex(largest->first).c_str(), Hex(largest->second).c_str());
    }

    void rethrowFailure() {
        std::lock_guard lock(failureMutex);
        if (failure) std::rethrow_exception(failure);
    }

    void apply(const Event& event) {
        try {
            applyEvent(event);
        } catch (const std::exception& error) {
            Fail("event " + std::to_string(&event - capture.events.data()) + " (type " + std::to_string(static_cast<std::uint32_t>(event.type)) + ", " + std::to_string(event.payload.size()) + " bytes): " + error.what());
        }
    }

    void applyEvent(const Event& event) {
        Reader reader(event.payload);
        switch (event.type) {
        case EventType::Begin:
        case EventType::Present:
        case EventType::End:
            break;
        case EventType::AddressSpace: {
            const auto backings = reader.GetSpan<Backing>();
            const auto removed = reader.GetSpan<Piece>();
            const auto added = reader.GetSpan<Piece>();
            const auto registryRemoved = reader.GetSpan<RegistryRange>();
            const auto registryAdded = reader.GetSpan<RegistryRange>();
            space.Apply(backings, removed, added, registryRemoved, registryAdded);
            ++mappingChanges;
            piecesMapped += added.size();
            piecesUnmapped += removed.size();
            break;
        }
        case EventType::Memory: {
            const auto kind = reader.Get<MemoryKind>();
            const auto runs = reader.GetSpan<MemoryRun>();
            for (std::uint32_t queue = 0; queue < QueueCount && TracePacingMs() >= 0; ++queue) {
                const auto awaited = ReplayQueueAwaited(queue) & ~std::uint64_t{3};
                if (awaited == 0) continue;
                Reader again(event.payload);
                static_cast<void>(again.Get<MemoryKind>());
                static_cast<void>(again.GetSpan<MemoryRun>());
                const auto indices = again.GetSpan<std::uint32_t>();
                std::size_t next = 0;
                for (const auto& run : runs) {
                    const auto end = run.address + static_cast<std::uint64_t>(run.pages) * PageBytes;
                    if (awaited >= run.address && awaited < end) {
                        const auto page = next + static_cast<std::size_t>((awaited - run.address) / PageBytes);
                        std::uint32_t value = 0;
                        if (page < indices.size()) std::memcpy(&value, capture.Page(indices[page]) + (awaited % PageBytes), sizeof(value));
                        std::fprintf(stderr, "[pacing] memory event writes queue %u's awaited 0x%llx: 0x%x -> 0x%x\n", queue, static_cast<unsigned long long>(awaited), *reinterpret_cast<const volatile std::uint32_t*>(static_cast<std::uintptr_t>(awaited)), value);
                    }
                    next += run.pages;
                }
            }
            const auto memoryStarted = Clock::now();
            {
                std::unique_lock replayWrites(ReplayMemoryWriteMutex());
                writeRuns(runs, reader.GetSpan<std::uint32_t>(), false, options.deltaMerge && kind == MemoryKind::Delta);
            }
            memoryMs += std::chrono::duration<double, std::milli>(Clock::now() - memoryStarted).count();
            for (const auto& run : runs) memoryBytes += static_cast<std::uint64_t>(run.pages) * PageBytes;
            if (kind == MemoryKind::Base || kind == MemoryKind::Mapped) space.ApplyProtections();
            break;
        }
        case EventType::Progress:
            pace(reader.GetSpan<std::uint64_t>());
            break;
        case EventType::Submit: {
            space.ApplyProtections();
            const auto submit = reader.Get<SubmitEvent>();
            const auto recorded = reader.GetSpan<std::uint32_t>();
            // A submission the capture carried unfinished records the driver's copy of its commands; the
            // game may have reused that command buffer before the capture began, so guest memory is
            // brought back to the recorded words.
            auto* guestWords = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(submit.packetAddress));
            if (recorded.size() == submit.packetWords && HashWords(std::span<const std::uint32_t>(guestWords, submit.packetWords)) != submit.hash && HashWords(recorded) == submit.hash) {
                std::memcpy(guestWords, recorded.data(), recorded.size_bytes());
            }
            if (TracePacingMs() >= 0) std::fprintf(stderr, "[pacing] submit queue %u, %u words at 0x%llx\n", submit.queue, submit.packetWords, static_cast<unsigned long long>(submit.packetAddress));
            // A carried submission's hash covers its guest words, which the driver's copy may stop short of
            // (at a REWIND, whose tail runs on its own): those words matching is the check.
            if (recorded.size() != submit.packetWords || HashWords(std::span<const std::uint32_t>(guestWords, submit.packetWords)) != submit.hash) ReplayExpectCommands(submit.hash);
            Packet packet{reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(submit.packetAddress)), submit.packetWords, 0, {}};
            AgcDriver::Submit(&packet, submit.queue);
            ++submits;
            rethrowFailure();
            break;
        }
        case EventType::RewindTail: {
            const auto tail = reader.Get<RewindTailEvent>();
            if (TracePacingMs() >= 0) std::fprintf(stderr, "[pacing] rewind tail queue %u, next tail 0x%llx\n", tail.queue, static_cast<unsigned long long>(tail.nextTail));
            ReplayPushRewindTail(tail.queue, reader.GetSpan<std::uint32_t>(), tail.nextTail, tail.nextWords);
            break;
        }
        case EventType::Suspend:
            AgcDriverSuspendPoint_nid_postfix();
            break;
        case EventType::Shader: {
            const auto codeAddress = reader.Get<std::uint64_t>();
            const auto headerAddress = reader.Get<std::uint64_t>();
            const auto type = reader.Get<std::uint8_t>();
            const auto code = reader.GetSpan<std::uint32_t>();
            ReplayRegisterShader(codeAddress, headerAddress, type, code, reader.GetSpan<std::byte>());
            break;
        }
        case EventType::QueueState: {
            const auto queue = reader.Get<std::uint32_t>();
            ReplayRestoreQueueState(queue, reader.GetSpan<std::byte>());
            break;
        }
        case EventType::DriverState: {
            const bool reset = reader.Get<std::uint8_t>() != 0;
            ReplayRestoreDriverState(reset, reader.GetSpan<std::byte>());
            break;
        }
        case EventType::VideoOutput: {
            const auto output = reader.Get<VideoOutputEvent>();
            if (output.registered != 0) {
                auto created = std::make_shared<ReplayOutput>(*this);
                AgcDriverRegisterVideoOutput_nid_postfix(output.handle, created);
                outputs[output.handle] = created;
            } else {
                const auto found = outputs.find(output.handle);
                if (found == outputs.end()) Fail("capture unregisters an unknown video output");
                AgcDriverUnregisterVideoOutput_nid_postfix(output.handle, found->second);
                outputs.erase(found);
            }
            break;
        }
        default:
            Fail("unknown capture event type " + std::to_string(static_cast<std::uint32_t>(event.type)));
        }
    }

    // A delta (`merge`) writes only what the capture changed in each page since its last captured version;
    // a base, a mapping or a restore writes whole pages.
    void writeRuns(std::span<const MemoryRun> runs, std::span<const std::uint32_t> indices, bool compareFirst, bool merge = false) {
        std::size_t total = 0;
        for (const auto& run : runs) total += run.pages;
        if (total > indices.size()) Fail("capture memory event has fewer pages than its runs");
        std::vector<std::uint64_t> addresses;
        addresses.reserve(total);
        for (const auto& run : runs) {
            if (compareFirst) AgcDriver::GuestMemory::FlushGpuWrites(run.address, static_cast<std::size_t>(run.pages) * PageBytes);
            for (std::uint32_t page = 0; page < run.pages; ++page) addresses.push_back(run.address + static_cast<std::uint64_t>(page) * PageBytes);
        }
        constexpr std::uint64_t NoPage = std::uint64_t{1} << 32u;
        std::vector<std::uint64_t> previous(total, NoPage);
        for (std::size_t i = 0; i < total; ++i) {
            auto& last = lastPage[addresses[i]];
            if (merge && last != 0) previous[i] = last - 1;
            last = std::uint64_t{indices[i]} + 1;
        }
        const auto writePage = [&](std::size_t i) {
            if (previous[i] == NoPage) {
                space.Write(addresses[i], capture.Page(indices[i]), PageBytes, compareFirst);
            } else if (previous[i] != indices[i]) {
                space.Write(addresses[i], capture.Page(indices[i]), PageBytes, false, true, capture.Page(static_cast<std::uint32_t>(previous[i])));
            }
        };
        if (!compareFirst && total >= ParallelWritePages) {
            // A large delta (a video frame, a streamed buffer): the game's threads wrote it alongside
            // the GPU work, and the first store to each watched page costs a write-watch fault, so
            // the pages are written by several threads. The event still completes before the next.
            writers.Run((total + WriteChunkPages - 1) / WriteChunkPages, [&](std::size_t chunk) {
                const auto end = std::min(total, (chunk + 1) * WriteChunkPages);
                for (auto i = chunk * WriteChunkPages; i < end; ++i) writePage(i);
            });
            return;
        }
        for (std::size_t i = 0; i < total; ++i) writePage(i);
    }

    void pace(std::span<const std::uint64_t> target) {
        if (!options.pacing) return;
        const auto started = Clock::now();
        std::optional<Clock::time_point> stalledAt;
        for (;;) {
            bool reached = true;
            for (std::size_t queue = 0; queue < target.size() && queue < QueueCount; ++queue) {
                if (target[queue] != 0 && ReplayPacketsExecuted(static_cast<std::uint32_t>(queue)) - base[queue] < target[queue]) {
                    reached = false;
                    break;
                }
            }
            if (reached) break;
            rethrowFailure();
            if (ReplayStalled()) {
                if (!stalledAt) stalledAt = Clock::now();
                else if (Clock::now() - *stalledAt > std::chrono::milliseconds(2)) {
                    ++stallReleases;
                    break;
                }
            } else {
                stalledAt.reset();
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        if (options.settle) ReplaySettle();
        const auto waitedMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        pacingMs += waitedMs;
        const double traceMs = TracePacingMs();
        if (traceMs >= 0 && waitedMs > traceMs) {
            std::string text;
            for (std::size_t queue = 0; queue < target.size() && queue < QueueCount; ++queue) {
                if (target[queue] == 0) continue;
                char item[96];
                std::snprintf(item, sizeof(item), " queue %zu %llu/%llu", queue, static_cast<unsigned long long>(ReplayPacketsExecuted(static_cast<std::uint32_t>(queue)) - base[queue]), static_cast<unsigned long long>(target[queue]));
                text += item;
                if (const auto awaited = ReplayQueueAwaited(static_cast<std::uint32_t>(queue)); awaited != 0) {
                    std::snprintf(item, sizeof(item), " (awaits 0x%llx = 0x%x)", static_cast<unsigned long long>(awaited), *reinterpret_cast<const volatile std::uint32_t*>(static_cast<std::uintptr_t>(awaited & ~std::uint64_t{3})));
                    text += item;
                }
            }
            std::fprintf(stderr, "[pacing] waited %.2f ms until%s\n", waitedMs, text.c_str());
        }
    }

    // --dump-range: the range's guest bytes (GPU writes flushed) into range_<address>_loop<n>_<when>.bin.
    void dumpRanges(const char* when) {
        for (const auto& [address, bytes] : options.dumpRanges) {
            AgcDriver::GuestMemory::FlushGpuWrites(address, static_cast<std::size_t>(bytes));
            char name[96];
            std::snprintf(name, sizeof(name), "range_%llx_loop%u_%s.bin", static_cast<unsigned long long>(address), loop, when);
            std::ofstream file(name, std::ios::binary);
            file.write(reinterpret_cast<const char*>(static_cast<std::uintptr_t>(address)), static_cast<std::streamsize>(bytes));
        }
    }

    bool drain() {
        std::optional<Clock::time_point> stalledAt;
        for (;;) {
            if (ReplayDrain(std::chrono::milliseconds(100))) return true;
            rethrowFailure();
            if (!ReplayStalled()) {
                stalledAt.reset();
                continue;
            }
            if (!stalledAt) stalledAt = Clock::now();
            else if (Clock::now() - *stalledAt > std::chrono::seconds(3)) return false;
        }
    }

    void restore() {
        ReplaySettle();
        if (options.cold != 0) {
            const auto started = Clock::now();
            ReplayClearCaches(options.cold);
            std::fprintf(stderr, "[replay] loop %u: driver caches cleared (classes 0x%x) in %.1f ms\n", loop, options.cold, std::chrono::duration<double, std::milli>(Clock::now() - started).count());
        }
        std::vector<Piece> removed;
        std::vector<Piece> added;
        std::set_difference(space.Pieces().begin(), space.Pieces().end(), initialPieces.begin(), initialPieces.end(), std::back_inserter(removed), PieceLess);
        std::set_difference(initialPieces.begin(), initialPieces.end(), space.Pieces().begin(), space.Pieces().end(), std::back_inserter(added), PieceLess);
        std::vector<RegistryRange> registryRemoved;
        std::vector<RegistryRange> registryAdded;
        std::set_difference(space.Registry().begin(), space.Registry().end(), initialRegistry.begin(), initialRegistry.end(), std::back_inserter(registryRemoved), RegistryLess);
        std::set_difference(initialRegistry.begin(), initialRegistry.end(), space.Registry().begin(), space.Registry().end(), std::back_inserter(registryAdded), RegistryLess);
        if (!removed.empty() || !added.empty() || !registryRemoved.empty() || !registryAdded.empty()) space.Apply({}, removed, added, registryRemoved, registryAdded);
        space.restoredPages = 0;
        space.restoredRanges.clear();
        for (std::size_t i = 0; i < prologueEnd; ++i) {
            const auto& event = capture.events[i];
            Reader reader(event.payload);
            if (event.type == EventType::Memory) {
                static_cast<void>(reader.Get<MemoryKind>());
                const auto runs = reader.GetSpan<MemoryRun>();
                writeRuns(runs, reader.GetSpan<std::uint32_t>(), true);
            } else if (event.type == EventType::QueueState || event.type == EventType::DriverState) {
                apply(event);
            }
        }
        lastPage = initialLastPage;
        space.ApplyProtections();
        // The restore rewrites every page the loop changed, several times what the game writes
        // between two frames: collected here, the write-watch walk and reset of those pages land in
        // the setup instead of the first frame's lookups. They are stamped as written as before.
        // --no-restore-collect leaves them to the frame.
        if (options.restoreCollect && !space.restoredRanges.empty()) {
            auto& ranges = space.restoredRanges;
            std::sort(ranges.begin(), ranges.end());
            std::vector<std::pair<std::uint64_t, std::uint64_t>> merged;
            for (const auto& range : ranges) {
                if (!merged.empty() && range.first <= merged.back().second) merged.back().second = std::max(merged.back().second, range.second);
                else merged.push_back(range);
            }
            ReplayCollectWrites(merged);
        }
        std::fprintf(stderr, "[replay] loop %u: restored %llu pages to the capture's start state\n", loop, static_cast<unsigned long long>(space.restoredPages));
    }

    const Options& options;
    const CaptureFile& capture;
    GuestSpace space;
    std::vector<Piece> initialPieces;
    std::vector<RegistryRange> initialRegistry;
    std::vector<std::optional<PresentEvent>> presents;
    std::size_t prologueEnd = 0;
    std::unique_ptr<Presenter> presenter;
    std::map<std::uint32_t, std::shared_ptr<ReplayOutput>> outputs;
    std::array<std::uint64_t, QueueCount> base{};
    std::atomic<std::uint64_t> nextFlip{0};
    std::mutex flipTimesMutex;
    std::vector<Clock::time_point> flipTimes;
    std::uint32_t loop = 0;
    double pacingMs = 0;
    static constexpr std::size_t ParallelWritePages = 256;
    static constexpr std::size_t WriteChunkPages = 64;
    WritePool writers{3};
    // The capture's memory deltas written during the loop (the game's CPU stores between submissions).
    double memoryMs = 0;
    std::uint64_t memoryBytes = 0;
    std::uint64_t stallReleases = 0;
    // Each written page's last captured version (page index + 1), at the loop's start and now.
    std::unordered_map<std::uint64_t, std::uint64_t> initialLastPage;
    std::unordered_map<std::uint64_t, std::uint64_t> lastPage;
    std::uint64_t submits = 0;
    std::uint64_t mappingChanges = 0;
    std::uint64_t piecesMapped = 0;
    std::uint64_t piecesUnmapped = 0;
    bool stuck = false;
    std::mutex failureMutex;
    std::exception_ptr failure;
};

void ReplayFlip::GpuReady(const std::shared_ptr<AgcDriver::FrameTiming>& timing) {
    owner.FlipReady(flip, timing);
}

void ReplayFlip::Fail(std::exception_ptr error) noexcept {
    owner.NoteFailure(error);
}

std::shared_ptr<AgcDriver::IFlipRequest> ReplayOutput::Reserve(const AgcDriver::FlipInfo&) {
    return owner.ReserveFlip();
}

void ReplayOutput::Fail(std::exception_ptr error) noexcept {
    owner.NoteFailure(error);
}

int Compare(const std::filesystem::path& replayed, const std::filesystem::path& reference, const std::vector<std::optional<PresentEvent>>& presents) {
    std::fprintf(stderr, "[compare] %s against %s\n", replayed.string().c_str(), reference.string().c_str());
    double worst = std::numeric_limits<double>::infinity();
    std::size_t compared = 0;
    std::size_t missing = 0;
    for (std::size_t flip = 0; flip < presents.size(); ++flip) {
        if (!presents[flip] || !presents[flip]->hasBuffer) continue;
        const auto ours = LoadPng(FramePath(replayed, 0, flip), std::chrono::seconds(30));
        const auto theirs = LoadPng(FramePath(reference, 0, flip), std::chrono::seconds(0));
        if (!ours || !theirs) {
            std::fprintf(stderr, "[compare] frame %zu: %s missing\n", flip, !ours ? "replayed frame" : "reference frame");
            ++missing;
            continue;
        }
        if (ours->width != theirs->width || ours->height != theirs->height) {
            std::fprintf(stderr, "[compare] frame %zu: size %dx%d differs from the reference %dx%d\n", flip, ours->width, ours->height, theirs->width, theirs->height);
            ++missing;
            continue;
        }
        std::uint32_t maximum = 0;
        double squared = 0;
        double absolute = 0;
        std::size_t differing = 0;
        for (std::size_t pixel = 0; pixel < ours->pixels.size() / 4; ++pixel) {
            std::uint32_t pixelMax = 0;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto difference = static_cast<std::uint32_t>(std::abs(static_cast<int>(ours->pixels[pixel * 4 + channel]) - static_cast<int>(theirs->pixels[pixel * 4 + channel])));
                pixelMax = std::max(pixelMax, difference);
                squared += static_cast<double>(difference) * difference;
                absolute += difference;
            }
            maximum = std::max(maximum, pixelMax);
            if (pixelMax > 8) ++differing;
        }
        const auto samples = static_cast<double>(ours->pixels.size() / 4 * 3);
        const auto mse = squared / samples;
        const auto psnr = mse == 0 ? std::numeric_limits<double>::infinity() : 10.0 * std::log10(255.0 * 255.0 / mse);
        worst = std::min(worst, psnr);
        ++compared;
        std::fprintf(stderr, "[compare] frame %zu: PSNR %.1f dB, mean |diff| %.2f, max %u, %.2f%% of pixels differ by more than 8\n", flip, psnr, absolute / samples, maximum, 100.0 * static_cast<double>(differing) / static_cast<double>(ours->pixels.size() / 4));
    }
    std::fprintf(stderr, "[compare] %zu frames compared, %zu missing; worst PSNR %.1f dB\n", compared, missing, worst);
    return missing == 0 ? 0 : 1;
}

std::uint32_t ParseCacheClasses(const std::string& list) {
    static const std::map<std::string, std::uint32_t> names{{"dispatch", CacheDispatch}, {"draw", CacheDraw}, {"resources", CacheResources}, {"textures", CacheTextures}, {"tables", CacheTables}, {"space", CacheSpace}, {"all", CacheAll}, {"live", CacheLive}};
    std::uint32_t classes = 0;
    std::size_t start = 0;
    while (start <= list.size()) {
        const auto end = std::min(list.find(',', start), list.size());
        const auto found = names.find(list.substr(start, end - start));
        if (found == names.end()) Fail("unknown cache class '" + list.substr(start, end - start) + "' (live, all, dispatch, draw, resources, textures, tables, space)");
        classes |= found->second;
        start = end + 1;
    }
    return classes;
}

Options ParseOptions(int argc, char** argv) {
    Options options;
    const auto usage = [] {
        Fail("usage: agc_frame_replay <capture dir> [--loop N] [--png DIR] [--png-scale N] [--png-all-loops] [--compare DIR] [--no-pacing] [--settle] [--hidden] [--shader-cache DIR] [--cold[=live|all|dispatch,draw,resources,textures,tables,space]]");
    };
    if (argc < 2) usage();
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) usage();
            return argv[++i];
        };
        if (argument == "--loop") options.loops = static_cast<std::uint32_t>(std::stoul(value()));
        else if (argument == "--for-seconds") options.forSeconds = std::stod(value());
        else if (argument == "--png") options.png = value();
        else if (argument == "--png-scale") options.pngScale = static_cast<std::uint32_t>(std::stoul(value()));
        else if (argument == "--png-all-loops") options.pngAllLoops = true;
        else if (argument == "--compare") options.compare = value();
        else if (argument == "--no-pacing") options.pacing = false;
        else if (argument == "--no-restore-collect") options.restoreCollect = false;
        else if (argument == "--no-delta-merge") options.deltaMerge = false;
        else if (argument == "--dump-range") {
            const auto text = value();
            const auto colon = text.find(':');
            if (colon == std::string::npos) Fail("--dump-range takes <hex address>:<hex bytes>");
            options.dumpRanges.emplace_back(std::stoull(text.substr(0, colon), nullptr, 16), std::stoull(text.substr(colon + 1), nullptr, 16));
        }
        else if (argument == "--settle") options.settle = true;
        else if (argument == "--hidden") options.hidden = true;
        else if (argument == "--shader-cache") options.shaderCache = value();
        else if (argument == "--cold") options.cold = CacheLive;
        else if (argument.starts_with("--cold=")) options.cold = ParseCacheClasses(argument.substr(7));
        else if (!argument.starts_with("--") && options.capture.empty()) options.capture = argument;
        else usage();
    }
    if (options.capture.empty() || options.loops == 0 || options.pngScale == 0) usage();
    if (options.png && !options.compare && std::filesystem::exists(options.capture / "live")) options.compare = options.capture / "live";
    return options;
}

}

void DisablePowerThrottling() {
    constexpr ULONG executionSpeed = 0x1;
    constexpr ULONG ignoreTimerResolution = 0x4;
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = executionSpeed | ignoreTimerResolution;
    state.StateMask = 0;
    if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state))) std::fprintf(stderr, "[replay] could not opt out of power throttling (error %lu); timings may vary\n", GetLastError());
}

int main(int argc, char** argv) {
    try {
        DisablePowerThrottling();
        const auto options = ParseOptions(argc, argv);
        if (options.shaderCache && _wputenv_s(L"ANYPS5_SHADER_CACHE_DIR", options.shaderCache->wstring().c_str()) != 0) Fail("cannot set ANYPS5_SHADER_CACHE_DIR");
        CaptureFile capture(options.capture);
        Replayer replayer(options, capture);
        replayer.Run();
        int status = replayer.Stuck() || ReplayCommandMismatches() != 0 ? 2 : 0;
        if (options.png && options.compare) status = std::max(status, Compare(*options.png, *options.compare, replayer.Presents()));
        std::fflush(stdout);
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(status));
        return status;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "agc_frame_replay: %s\n", error.what());
        return 1;
    }
}
