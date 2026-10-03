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
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
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
    std::optional<std::filesystem::path> png;
    std::uint32_t pngScale = 1;
    bool pngAllLoops = false;
    std::optional<std::filesystem::path> compare;
    bool pacing = true;
    bool settle = false;
    bool hidden = false;
    std::optional<std::filesystem::path> shaderCache;
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
            for (const auto& range : registryAdded) mutation.Add(reinterpret_cast<void*>(range.address), static_cast<std::size_t>(range.bytes), range.readable != 0, range.writable != 0, range.sceProtection);
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

    void Write(std::uint64_t address, const std::byte* source, std::size_t bytes, bool compareFirst) {
        auto* target = reinterpret_cast<std::byte*>(address);
        if (compareFirst) {
            const bool same = source != nullptr ? std::memcmp(target, source, bytes) == 0 : std::all_of(target, target + bytes, [](std::byte value) { return value == std::byte{0}; });
            if (same) return;
            ++restoredPages;
        }
        const auto* piece = find(address);
        if (piece == nullptr) Fail("capture writes " + Hex(address) + " outside every mapped piece");
        const bool applied = std::find(pendingProtections.begin(), pendingProtections.end(), *piece) == pendingProtections.end();
        if (!applied || piece->writable) {
            copy(target, source, bytes);
            return;
        }
        const auto page = address & ~(ViewBytes - 1);
        DWORD previous;
        if (!VirtualProtect(reinterpret_cast<void*>(page), ViewBytes, PAGE_READWRITE, &previous)) Fail("cannot open read-only guest page " + Hex(page) + " for a capture write");
        copy(target, source, bytes);
        if (!VirtualProtect(reinterpret_cast<void*>(page), ViewBytes, previous, &previous)) Fail("cannot restore guest page protection at " + Hex(page));
    }

    const std::vector<Piece>& Pieces() const { return pieces; }
    const std::vector<RegistryRange>& Registry() const { return registry; }
    std::uint64_t restoredPages = 0;

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

class Replayer;

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
        presenter = std::make_unique<Presenter>(width, height, options.hidden);
        presenter->dumpScale = options.pngScale;
        presenter->Enqueue({true, std::nullopt, {}, nullptr});
        presenter->WaitDone();
        for (loop = 0; loop < options.loops; ++loop) {
            const auto started = Clock::now();
            if (loop == 0) {
                for (std::size_t i = 0; i < prologueEnd; ++i) apply(capture.events[i]);
                initialPieces = space.Pieces();
                initialRegistry = space.Registry();
            } else {
                restore();
            }
            for (std::size_t queue = 0; queue < QueueCount; ++queue) base[queue] = ReplayPacketsExecuted(static_cast<std::uint32_t>(queue));
            nextFlip.store(0);
            const auto presentedBefore = presenter->Presented();
            const auto replayStarted = Clock::now();
            pacingMs = 0;
            stallReleases = 0;
            submits = 0;
            for (std::size_t i = prologueEnd; i < capture.events.size(); ++i) apply(capture.events[i]);
            const bool finished = drain();
            presenter->WaitDone();
            rethrowFailure();
            const auto replayMs = std::chrono::duration<double, std::milli>(Clock::now() - replayStarted).count();
            const auto setupMs = std::chrono::duration<double, std::milli>(replayStarted - started).count();
            const auto frames = presenter->Presented() - presentedBefore;
            std::fprintf(stderr, "[replay] loop %u: %llu submissions, %llu frames in %.1f ms (%.2f FPS); setup %.1f ms; pacing waits %.1f ms, %llu stall releases; command mismatches %llu%s\n", loop, static_cast<unsigned long long>(submits), static_cast<unsigned long long>(frames), replayMs, frames != 0 ? 1000.0 * static_cast<double>(frames) / replayMs : 0.0, setupMs, pacingMs, static_cast<unsigned long long>(stallReleases), static_cast<unsigned long long>(ReplayCommandMismatches()), finished ? "" : "; QUEUES STILL BLOCKED at the end");
            if (!finished) {
                stuck = true;
                break;
            }
        }
        ReplaySettle();
    }

    std::shared_ptr<AgcDriver::IFlipRequest> ReserveFlip() {
        return std::make_shared<ReplayFlip>(*this, nextFlip.fetch_add(1));
    }

    void FlipReady(std::uint64_t flip, const std::shared_ptr<AgcDriver::FrameTiming>& timing) {
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
            break;
        }
        case EventType::Memory: {
            const auto kind = reader.Get<MemoryKind>();
            const auto runs = reader.GetSpan<MemoryRun>();
            writeRuns(runs, reader.GetSpan<std::uint32_t>(), false);
            if (kind == MemoryKind::Base || kind == MemoryKind::Mapped) space.ApplyProtections();
            break;
        }
        case EventType::Progress:
            pace(reader.GetSpan<std::uint64_t>());
            break;
        case EventType::Submit: {
            space.ApplyProtections();
            const auto submit = reader.Get<SubmitEvent>();
            static_cast<void>(reader.GetSpan<std::uint32_t>());
            ReplayExpectCommands(submit.hash);
            Packet packet{reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(submit.packetAddress)), submit.packetWords, 0, {}};
            AgcDriver::Submit(&packet, submit.queue);
            ++submits;
            rethrowFailure();
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

    void writeRuns(std::span<const MemoryRun> runs, std::span<const std::uint32_t> indices, bool compareFirst) {
        std::size_t next = 0;
        for (const auto& run : runs) {
            if (compareFirst) AgcDriver::GuestMemory::FlushGpuWrites(run.address, static_cast<std::size_t>(run.pages) * PageBytes);
            for (std::uint32_t page = 0; page < run.pages; ++page) {
                if (next >= indices.size()) Fail("capture memory event has fewer pages than its runs");
                space.Write(run.address + static_cast<std::uint64_t>(page) * PageBytes, capture.Page(indices[next++]), PageBytes, compareFirst);
            }
        }
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
        pacingMs += std::chrono::duration<double, std::milli>(Clock::now() - started).count();
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
        space.ApplyProtections();
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
    std::uint32_t loop = 0;
    double pacingMs = 0;
    std::uint64_t stallReleases = 0;
    std::uint64_t submits = 0;
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

Options ParseOptions(int argc, char** argv) {
    Options options;
    const auto usage = [] {
        Fail("usage: agc_frame_replay <capture dir> [--loop N] [--png DIR] [--png-scale N] [--png-all-loops] [--compare DIR] [--no-pacing] [--settle] [--hidden] [--shader-cache DIR]");
    };
    if (argc < 2) usage();
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) usage();
            return argv[++i];
        };
        if (argument == "--loop") options.loops = static_cast<std::uint32_t>(std::stoul(value()));
        else if (argument == "--png") options.png = value();
        else if (argument == "--png-scale") options.pngScale = static_cast<std::uint32_t>(std::stoul(value()));
        else if (argument == "--png-all-loops") options.pngAllLoops = true;
        else if (argument == "--compare") options.compare = value();
        else if (argument == "--no-pacing") options.pacing = false;
        else if (argument == "--settle") options.settle = true;
        else if (argument == "--hidden") options.hidden = true;
        else if (argument == "--shader-cache") options.shaderCache = value();
        else if (!argument.starts_with("--") && options.capture.empty()) options.capture = argument;
        else usage();
    }
    if (options.capture.empty() || options.loops == 0 || options.pngScale == 0) usage();
    if (options.png && !options.compare && std::filesystem::exists(options.capture / "live")) options.compare = options.capture / "live";
    return options;
}

}

int main(int argc, char** argv) {
    try {
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
