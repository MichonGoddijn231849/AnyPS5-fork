#include "prx/libSceAgcDriver/Execution/include/Driver/Capture/FrameCapture.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libkernel/DirectMemory/DirectMemory.hpp"
#include <limits>
#include <algorithm>
#include <bit>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <tuple>

namespace AgcDriver::Capture {

namespace {

constexpr std::size_t ChunkBytes = 16u * 1024u * 1024u;

auto PieceKey(const Piece& piece) {
    return std::tie(piece.address, piece.bytes, piece.kind, piece.backing, piece.backingOffset, piece.readable, piece.writable, piece.gpu, piece.memoryType);
}

auto RegistryKey(const RegistryRange& range) {
    return std::tie(range.address, range.bytes, range.readable, range.writable, range.gpu, range.sceProtection);
}

template<typename T, typename TKey>
std::vector<T> Difference(const std::vector<T>& from, const std::vector<T>& minus, TKey key) {
    std::vector<T> result;
    std::set_difference(from.begin(), from.end(), minus.begin(), minus.end(), std::back_inserter(result), [&](const T& a, const T& b) { return key(a) < key(b); });
    return result;
}

Hash128 HashPage(const std::byte* page) {
    std::uint64_t a = 0x243f6a8885a308d3ull;
    std::uint64_t b = 0x13198a2e03707344ull;
    for (std::size_t i = 0; i < PageBytes / 8; ++i) {
        std::uint64_t word;
        std::memcpy(&word, page + i * 8, 8);
        a = (a ^ word) * 0x9e3779b97f4a7c15ull;
        a ^= a >> 31u;
        b = std::rotl((b + word) * 0xc2b2ae3d27d4eb4full, 29) ^ a;
    }
    return {a, b};
}

bool ZeroPageBytes(const std::byte* page) {
    for (std::size_t i = 0; i < PageBytes / 8; ++i) {
        std::uint64_t word;
        std::memcpy(&word, page + i * 8, 8);
        if (word != 0) return false;
    }
    return true;
}

std::pair<std::uint64_t, std::uint64_t> ParseFrames(const char* text) {
    if (text == nullptr) return {60, 62};
    char* end = nullptr;
    const auto first = std::strtoull(text, &end, 10);
    if (end == text) throw std::invalid_argument("APS5_CAPTURE_FRAMES must be <first>-<last>");
    if (*end == '\0') return {first, first};
    if (*end != '-') throw std::invalid_argument("APS5_CAPTURE_FRAMES must be <first>-<last>");
    const char* lastText = end + 1;
    const auto last = std::strtoull(lastText, &end, 10);
    if (end == lastText || *end != '\0' || last < first) throw std::invalid_argument("APS5_CAPTURE_FRAMES must be <first>-<last> with last >= first");
    return {first, last};
}

double Seconds(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

double Gib(std::uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
}

double Mib(std::uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

}

FrameCapture& FrameCapture::Get() {
    static FrameCapture capture;
    return capture;
}

FrameCapture::FrameCapture() {
    const char* target = std::getenv("APS5_CAPTURE");
    if (target == nullptr || *target == '\0') return;
    directory = target;
    std::tie(firstFrame, lastFrame) = ParseFrames(std::getenv("APS5_CAPTURE_FRAMES"));
    const char* limit = std::getenv("APS5_CAPTURE_MAX_GB");
    limitBytes = (limit != nullptr ? std::strtoull(limit, nullptr, 10) : 20ull) << 30u;
    if (limitBytes == 0) throw std::invalid_argument("APS5_CAPTURE_MAX_GB must be positive");
    nextAttempt = firstFrame;
    if (const char* trigger = std::getenv("APS5_CAPTURE_TRIGGER"); trigger != nullptr && *trigger != '    nextAttempt = firstFrame;') {
        triggerPath = trigger;
        nextAttempt = std::numeric_limits<std::uint64_t>::max();
    }
    std::fprintf(stderr, "[frame-capture] armed: frames %llu-%llu into %s (limit %.0f GiB)\n", static_cast<unsigned long long>(firstFrame), static_cast<unsigned long long>(lastFrame), directory.c_str(), Gib(limitBytes));
    phase.store(Phase::Waiting, std::memory_order_release);
}

void FrameCapture::PollTrigger() {
    if (triggerPath.empty() || nextAttempt != std::numeric_limits<std::uint64_t>::max()) return;
    std::error_code error;
    if (!std::filesystem::exists(triggerPath, error)) return;
    std::filesystem::remove(triggerPath, error);
    const auto span = lastFrame - firstFrame;
    firstFrame = flips + 1;
    lastFrame = firstFrame + span;
    nextAttempt = firstFrame;
    std::fprintf(stderr, "[frame-capture] triggered: frames %llu-%llu\n", static_cast<unsigned long long>(firstFrame), static_cast<unsigned long long>(lastFrame));
}

void FrameCapture::Begin(std::uint64_t flipsBeforeStart) {
    startedAt = std::chrono::steady_clock::now();
    flipsBefore = flipsBeforeStart;
    std::filesystem::create_directories(std::filesystem::path(directory) / "live");
    events = std::fopen((std::filesystem::path(directory) / "events.bin").string().c_str(), "wb");
    pages = std::fopen((std::filesystem::path(directory) / "pages.bin").string().c_str(), "wb");
    if (events == nullptr || pages == nullptr) throw CaptureFailed("cannot create the capture files in " + directory);
    std::setvbuf(events, nullptr, _IOFBF, 8u << 20u);
    std::setvbuf(pages, nullptr, _IOFBF, 8u << 20u);
    std::fwrite(Magic.data(), 1, Magic.size(), events);
    const std::uint32_t header[2] = {Version, 0};
    std::fwrite(header, sizeof(header), 1, events);
    eventBytes = Magic.size() + sizeof(header);
    Writer begin;
    begin.Put(BeginEvent{firstFrame, lastFrame, flipsBefore, 0});
    RecordEvent(EventType::Begin, begin.data);

    GuestMemory::SetCaptureDirtyPages(true);
    registryGeneration = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    std::vector<Backing> backings;
    current = snapshotAddressSpace(backings);
    Writer space;
    space.PutSpan<Backing>(backings);
    space.PutSpan<Piece>({});
    space.PutSpan<Piece>(current.pieces);
    space.PutSpan<RegistryRange>({});
    space.PutSpan<RegistryRange>(current.registry);
    RecordEvent(EventType::AddressSpace, space.data);

    const auto collectStart = std::chrono::steady_clock::now();
    for (const auto& piece : current.pieces) {
        if (piece.kind != PieceKind::External && piece.gpu && piece.readable && piece.writable) GuestMemory::CollectWritesUncached(piece.address, static_cast<std::size_t>(piece.bytes));
    }
    static_cast<void>(GuestMemory::TakeCaptureDirtyPages());
    collectSeconds += Seconds(collectStart);

    const auto baseStart = std::chrono::steady_clock::now();
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    for (const auto& piece : current.pieces) {
        if (!piece.gpu || !piece.readable) continue;
        for (const auto& range : GuestMemory::CommittedRanges(piece.address, static_cast<std::size_t>(piece.bytes))) ranges.push_back(range);
    }
    recordRanges(MemoryKind::Base, ranges, true);
    baseSeconds = Seconds(baseStart);
    std::fprintf(stderr, "[frame-capture] base: %llu pages (%.2f GiB, %llu zero), %u stored (%.2f GiB) in %.1f s\n", static_cast<unsigned long long>(base.pages), Gib(base.pages * PageBytes), static_cast<unsigned long long>(base.zeroPages), storedPages, Gib(static_cast<std::uint64_t>(storedPages) * PageBytes), baseSeconds);
    phase.store(Phase::Recording, std::memory_order_release);
}

AddressSpace FrameCapture::snapshotAddressSpace(std::vector<Backing>& added) {
    AddressSpace space;
    std::uintptr_t arenaBase = 0;
    std::size_t arenaBytes = 0;
    GuestArena::GuestArenaRange_nid_postfix(&arenaBase, &arenaBytes);
    std::vector<DirectMappingInfo> direct;
    DirectMemoryMappings_nid_postfix(&direct);
    std::vector<GuestAllocations::Range> ranges;
    {
        const auto lease = GuestAllocations::GuestAllocationsAcquire_nid_postfix();
        ranges.reserve(lease.size());
        for (const auto& range : lease) ranges.push_back(*range);
    }
    std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) { return a.address < b.address; });
    const auto backingId = [&](const DirectMappingInfo& mapping) {
        const auto [it, inserted] = backingIds.emplace(mapping.backing, static_cast<std::uint32_t>(backingIds.size()));
        if (inserted) added.push_back({it->second, 0, mapping.backingBytes});
        return it->second;
    };
    for (const auto& range : ranges) {
        // This branch keeps no SCE protection per range: every range counts as GPU-visible.
        const bool gpu = range.readable || range.writable;
        space.registry.push_back({range.address, range.bytes, range.readable, range.writable, gpu, 0, -1});
        const auto end = range.address + range.bytes;
        auto mapping = std::upper_bound(direct.begin(), direct.end(), range.address, [](std::uint64_t address, const DirectMappingInfo& info) { return address < info.address; });
        if (mapping != direct.begin() && std::prev(mapping)->end > range.address) --mapping;
        for (auto cursor = range.address; cursor < end;) {
            Piece piece{};
            piece.address = cursor;
            piece.readable = range.readable;
            piece.writable = range.writable;
            piece.gpu = gpu;
            piece.memoryType = -1;
            if (mapping != direct.end() && mapping->address <= cursor && cursor < mapping->end) {
                const auto stop = std::min<std::uint64_t>(end, mapping->end);
                piece.kind = PieceKind::Direct;
                piece.bytes = stop - cursor;
                piece.backing = backingId(*mapping);
                piece.backingOffset = mapping->backingOffset + (cursor - mapping->address);
                piece.memoryType = mapping->memoryType;
                if (stop == mapping->end) ++mapping;
            } else {
                const auto stop = mapping != direct.end() && mapping->address < end ? std::max<std::uint64_t>(cursor, mapping->address) : end;
                const bool inArena = cursor >= arenaBase && stop <= arenaBase + arenaBytes;
                piece.kind = inArena ? PieceKind::Private : PieceKind::External;
                piece.bytes = stop - cursor;
            }
            if (piece.bytes == 0) throw CaptureFailed("address space snapshot made an empty piece");
            cursor += piece.bytes;
            space.pieces.push_back(piece);
        }
    }
    std::sort(space.pieces.begin(), space.pieces.end(), [](const Piece& a, const Piece& b) { return PieceKey(a) < PieceKey(b); });
    std::sort(space.registry.begin(), space.registry.end(), [](const RegistryRange& a, const RegistryRange& b) { return RegistryKey(a) < RegistryKey(b); });
    return space;
}

void FrameCapture::RecordAddressSpaceChanges() {
    const auto generation = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    if (generation == registryGeneration) return;
    registryGeneration = generation;
    std::vector<Backing> backings;
    auto next = snapshotAddressSpace(backings);
    const auto removed = Difference(current.pieces, next.pieces, PieceKey);
    const auto added = Difference(next.pieces, current.pieces, PieceKey);
    const auto registryRemoved = Difference(current.registry, next.registry, RegistryKey);
    const auto registryAdded = Difference(next.registry, current.registry, RegistryKey);
    current = std::move(next);
    if (removed.empty() && added.empty() && registryRemoved.empty() && registryAdded.empty()) return;
    Writer space;
    space.PutSpan<Backing>(backings);
    space.PutSpan<Piece>(removed);
    space.PutSpan<Piece>(added);
    space.PutSpan<RegistryRange>(registryRemoved);
    space.PutSpan<RegistryRange>(registryAdded);
    RecordEvent(EventType::AddressSpace, space.data);
    ++addressSpaceChanges;
    piecesAdded += added.size();
    piecesRemoved += removed.size();
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    for (const auto& piece : added) {
        if (!piece.gpu || !piece.readable) continue;
        for (const auto& range : GuestMemory::CommittedRanges(piece.address, static_cast<std::size_t>(piece.bytes))) ranges.push_back(range);
    }
    if (!ranges.empty()) recordRanges(MemoryKind::Mapped, ranges, false);
}

void FrameCapture::RecordDelta(std::span<const std::uint64_t> progress) {
    Writer marker;
    marker.PutSpan(progress);
    RecordEvent(EventType::Progress, marker.data);
    RecordAddressSpaceChanges();
    const auto collectStart = std::chrono::steady_clock::now();
    for (const auto& piece : current.pieces) {
        if (piece.kind != PieceKind::External && piece.gpu && piece.readable && piece.writable) GuestMemory::CollectWritesUncached(piece.address, static_cast<std::size_t>(piece.bytes));
    }
    auto dirty = GuestMemory::TakeCaptureDirtyPages();
    collectSeconds += Seconds(collectStart);
    std::sort(dirty.begin(), dirty.end());
    dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
    std::vector<std::uint64_t> kept;
    kept.reserve(dirty.size());
    std::vector<std::pair<std::uint64_t, std::uint64_t>> visible;
    for (const auto& piece : current.pieces) {
        if (piece.gpu && piece.readable) visible.emplace_back(piece.address, piece.address + piece.bytes);
    }
    std::sort(visible.begin(), visible.end());
    for (const auto page : dirty) {
        const auto aligned = page & ~static_cast<std::uint64_t>(PageBytes - 1);
        auto it = std::upper_bound(visible.begin(), visible.end(), std::pair<std::uint64_t, std::uint64_t>{aligned, ~std::uint64_t{0}});
        if (it == visible.begin()) continue;
        --it;
        if (aligned >= it->first && aligned + PageBytes <= it->second && (kept.empty() || kept.back() != aligned)) kept.push_back(aligned);
    }
    ++deltaPoints;
    if (!kept.empty()) recordPages(MemoryKind::Delta, kept);
}

void FrameCapture::RecordShader(std::uint64_t codeAddress, std::uint64_t headerAddress, std::uint8_t type, std::span<const std::uint32_t> code, std::span<const std::byte> header) {
    Writer shaderEvent;
    shaderEvent.Put(codeAddress);
    shaderEvent.Put(headerAddress);
    shaderEvent.Put(type);
    shaderEvent.PutSpan(code);
    shaderEvent.PutSpan(header);
    RecordEvent(EventType::Shader, shaderEvent.data);
    ++shaders;
    shaderBytes += code.size_bytes() + header.size();
}

void FrameCapture::RecordSubmit(const SubmitEvent& submit, std::span<const std::uint32_t> words) {
    Writer event;
    event.Put(submit);
    event.PutSpan(words);
    RecordEvent(EventType::Submit, event.data);
    submitWords += words.size();
    ++submitsPerQueue[submit.queue];
}

void FrameCapture::NotePresent(const DisplayBuffer* buffer, bool opaque, std::string& dumpPath) {
    if (phase.load(std::memory_order_acquire) == Phase::Off) return;
    const auto present = presentsSeen.fetch_add(1, std::memory_order_acq_rel);
    if (!Recording() || present < flipsBefore) return;
    PresentEvent event{};
    event.flip = present - flipsBefore;
    event.hasBuffer = buffer != nullptr;
    event.opaque = opaque;
    if (buffer != nullptr) {
        event.buffer = {buffer->address, buffer->pixelFormat, buffer->width, buffer->height, buffer->tilingMode, buffer->pitchInPixel};
        char name[32];
        std::snprintf(name, sizeof(name), "frame_%05llu.png", static_cast<unsigned long long>(event.flip));
        dumpPath = (std::filesystem::path(directory) / "live" / name).string();
    }
    Writer writer;
    writer.Put(event);
    RecordEvent(EventType::Present, writer.data);
}

bool FrameCapture::PresentsReached(std::uint64_t flipsEnd) const {
    return presentsSeen.load(std::memory_order_acquire) >= flipsEnd;
}

void FrameCapture::RecordEvent(EventType type, std::span<const std::byte> payload) {
    std::lock_guard lock(writerMutex);
    if (events == nullptr) throw CaptureFailed("capture event written after the capture closed");
    const EventHeader header{type, 0, payload.size()};
    if (std::fwrite(&header, sizeof(header), 1, events) != 1 || (!payload.empty() && std::fwrite(payload.data(), 1, payload.size(), events) != payload.size())) throw CaptureFailed("writing events.bin failed (disk full?)");
    eventBytes += sizeof(header) + payload.size();
    checkLimit();
}

std::uint32_t FrameCapture::storePage(const std::byte* page) {
    if (ZeroPageBytes(page)) return ZeroPage;
    const auto hash = HashPage(page);
    std::lock_guard lock(writerMutex);
    if (const auto found = pageIndex.find(hash); found != pageIndex.end()) return found->second;
    if (storedPages == ZeroPage - 1) throw CaptureFailed("capture page store is full");
    if (std::fwrite(page, 1, PageBytes, pages) != PageBytes) throw CaptureFailed("writing pages.bin failed (disk full?)");
    pageIndex.emplace(hash, storedPages);
    checkLimit();
    return storedPages++;
}

void FrameCapture::checkLimit() {
    const auto total = eventBytes + static_cast<std::uint64_t>(storedPages) * PageBytes;
    if (total > limitBytes) {
        char text[160];
        std::snprintf(text, sizeof(text), "capture exceeds its size limit: %.2f GiB > %.0f GiB (APS5_CAPTURE_MAX_GB)", Gib(total), Gib(limitBytes));
        throw CaptureFailed(text);
    }
}

void FrameCapture::recordRanges(MemoryKind kind, std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges, bool flush) {
    std::vector<MemoryRun> runs;
    std::vector<std::uint32_t> indices;
    std::vector<std::byte> buffer(ChunkBytes);
    auto& counts = kind == MemoryKind::Base ? base : mapped;
    const auto before = storedPages;
    for (const auto& [begin, end] : ranges) {
        const auto first = begin & ~static_cast<std::uint64_t>(PageBytes - 1);
        const auto stop = (end + PageBytes - 1) & ~static_cast<std::uint64_t>(PageBytes - 1);
        for (auto chunk = first; chunk < stop; chunk += ChunkBytes) {
            const auto bytes = static_cast<std::size_t>(std::min<std::uint64_t>(ChunkBytes, stop - chunk));
            if (flush) GuestMemory::FlushGpuWrites(chunk, bytes);
            const auto view = std::span(buffer).first(bytes);
            const bool whole = GuestMemory::CopyMapped(chunk, view) == GuestMemory::Compare::Equal;
            MemoryRun* run = nullptr;
            for (std::size_t offset = 0; offset < bytes; offset += PageBytes) {
                if (!whole && GuestMemory::CopyMapped(chunk + offset, view.subspan(offset, PageBytes)) != GuestMemory::Compare::Equal) {
                    run = nullptr;
                    continue;
                }
                const auto index = storePage(view.data() + offset);
                ++counts.pages;
                if (index == ZeroPage) ++counts.zeroPages;
                if (run == nullptr) {
                    runs.push_back({chunk + offset, 0, 0});
                    run = &runs.back();
                }
                ++run->pages;
                indices.push_back(index);
            }
        }
    }
    counts.storedPages += storedPages - before;
    Writer event;
    event.Put(kind);
    event.PutSpan<MemoryRun>(runs);
    event.PutSpan<std::uint32_t>(indices);
    RecordEvent(EventType::Memory, event.data);
}

void FrameCapture::recordPages(MemoryKind kind, std::span<const std::uint64_t> pageList) {
    std::vector<MemoryRun> runs;
    std::vector<std::uint32_t> indices;
    std::array<std::byte, PageBytes> page{};
    const auto frame = static_cast<std::size_t>(flips - flipsBefore);
    if (deltaPerFrame.size() <= frame) deltaPerFrame.resize(frame + 1);
    auto& counts = deltaPerFrame[frame];
    const auto before = storedPages;
    for (const auto address : pageList) {
        if (GuestMemory::CopyMapped(address, page) != GuestMemory::Compare::Equal) continue;
        const auto index = storePage(page.data());
        ++counts.pages;
        if (index == ZeroPage) ++counts.zeroPages;
        if (runs.empty() || runs.back().address + static_cast<std::uint64_t>(runs.back().pages) * PageBytes != address) runs.push_back({address, 0, 0});
        ++runs.back().pages;
        indices.push_back(index);
    }
    counts.storedPages += storedPages - before;
    Writer event;
    event.Put(kind);
    event.PutSpan<MemoryRun>(runs);
    event.PutSpan<std::uint32_t>(indices);
    RecordEvent(EventType::Memory, event.data);
}

void FrameCapture::Finish(const EndEvent& end) {
    Writer event;
    event.Put(end);
    RecordEvent(EventType::End, event.data);
    GuestMemory::SetCaptureDirtyPages(false);
    writeSummary("complete", "");
    closeFiles();
    phase.store(Phase::Done, std::memory_order_release);
    std::fprintf(stderr, "[frame-capture] complete: %llu flips, %llu submissions into %s\n", static_cast<unsigned long long>(end.flips), static_cast<unsigned long long>(end.submissions), directory.c_str());
}

void FrameCapture::Fail(const std::string& reason) {
    std::fprintf(stderr, "[frame-capture] FAILED: %s\n", reason.c_str());
    GuestMemory::SetCaptureDirtyPages(false);
    try {
        std::filesystem::create_directories(directory);
        writeSummary("FAILED", reason);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[frame-capture] cannot write capture.txt: %s\n", error.what());
    }
    closeFiles();
    phase.store(Phase::Done, std::memory_order_release);
}

void FrameCapture::closeFiles() {
    std::lock_guard lock(writerMutex);
    if (events != nullptr) std::fclose(events);
    if (pages != nullptr) std::fclose(pages);
    events = nullptr;
    pages = nullptr;
}

void FrameCapture::writeSummary(const char* status, const std::string& detail) {
    std::FILE* file = std::fopen((std::filesystem::path(directory) / "capture.txt").string().c_str(), "w");
    if (file == nullptr) throw std::runtime_error("cannot create capture.txt");
    std::uint64_t pagesBytes;
    std::uint64_t eventsTotal;
    {
        std::lock_guard lock(writerMutex);
        pagesBytes = static_cast<std::uint64_t>(storedPages) * PageBytes;
        eventsTotal = eventBytes;
    }
    std::fprintf(file, "status: %s%s%s\n", status, detail.empty() ? "" : ": ", detail.c_str());
    std::fprintf(file, "frames: %llu-%llu (capture starts after flip %llu)\n", static_cast<unsigned long long>(firstFrame), static_cast<unsigned long long>(lastFrame), static_cast<unsigned long long>(flipsBefore));
    std::fprintf(file, "submissions:");
    for (const auto& [queue, count] : submitsPerQueue) std::fprintf(file, " queue 0x%x: %llu", queue, static_cast<unsigned long long>(count));
    std::fprintf(file, "; command words %.2f MiB; shaders recorded (registered before or during the capture) %llu\n", Mib(submitWords * 4), static_cast<unsigned long long>(shaders));
    std::fprintf(file, "address space: %zu pieces, %zu registry ranges, %llu changes (+%llu/-%llu pieces), %zu backings\n", current.pieces.size(), current.registry.size(), static_cast<unsigned long long>(addressSpaceChanges), static_cast<unsigned long long>(piecesAdded), static_cast<unsigned long long>(piecesRemoved), backingIds.size());
    std::fprintf(file, "size: base %.2f GiB of GPU-visible pages (%.2f GiB zero) -> %.2f GiB stored; pages.bin %.2f GiB; events.bin %.1f MiB; total %.2f GiB\n", Gib(base.pages * PageBytes), Gib(base.zeroPages * PageBytes), Gib(base.storedPages * PageBytes), Gib(pagesBytes), Mib(eventsTotal), Gib(pagesBytes + eventsTotal));
    std::fprintf(file, "mapped during capture: %.1f MiB (%.1f MiB stored); shader code and headers %.1f MiB\n", Mib(mapped.pages * PageBytes), Mib(mapped.storedPages * PageBytes), Mib(shaderBytes));
    std::fprintf(file, "deltas per frame (%llu delta points):", static_cast<unsigned long long>(deltaPoints));
    for (std::size_t frame = 0; frame < deltaPerFrame.size(); ++frame) std::fprintf(file, " [%zu] %.1f MiB written, %.1f MiB new;", frame, Mib(deltaPerFrame[frame].pages * PageBytes), Mib(deltaPerFrame[frame].storedPages * PageBytes));
    std::fprintf(file, "\n");
    std::fprintf(file, "time: base snapshot %.1f s, write-watch collects %.1f s, capture wall %.1f s\n", baseSeconds, collectSeconds, startedAt == std::chrono::steady_clock::time_point{} ? 0.0 : Seconds(startedAt));
    std::fclose(file);
}

}
