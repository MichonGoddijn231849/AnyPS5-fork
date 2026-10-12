#include "prx/libSceAgcDriver/Execution/include/Driver/Capture/PassCapture.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Capture/PassFiles.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string_view>

namespace AgcDriver::DriverDetail {

namespace {

struct Plan {
    std::vector<std::unique_ptr<PassCapture::Pass>> passes;
    std::filesystem::path blobs;
    std::uint64_t budget = ~std::uint64_t{0};
    std::atomic<std::uint64_t> written{0};
    std::mutex blobMutex;
};

Plan& Passes() {
    static Plan* const plan = [] {
        auto* parsed = new Plan;
        const char* path = std::getenv("APS5_CAPTURE_PASSES");
        if (path == nullptr) return parsed;
        if (const char* budget = std::getenv("APS5_CAPTURE_PASSES_BUDGET_MIB")) parsed->budget = std::strtoull(budget, nullptr, 10) << 20u;
        parsed->blobs = std::filesystem::path(std::string(path) + ".blobs");
        std::ifstream file(path);
        if (!file) throw std::runtime_error(std::string("APS5_CAPTURE_PASSES: cannot read ") + path);
        for (std::string line; std::getline(file, line);) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty()) continue;
            auto pass = std::make_unique<PassCapture::Pass>();
            char directory[1024];
            int used = 0;
            unsigned long long first = 0, last = 0;
            if (std::sscanf(line.c_str(), "draws %llu %llu %1023s%n", &first, &last, directory, &used) == 3) {
                pass->draws = true;
                pass->first = first;
                pass->last = last;
            } else if (std::sscanf(line.c_str(), "%llx %llu %1023s%n", &first, &last, directory, &used) == 3) {
                pass->program = first;
                pass->first = pass->last = last;
                for (const char* cursor = line.c_str() + used; *cursor != 0;) {
                    while (*cursor == ' ' || *cursor == ',') ++cursor;
                    unsigned long long address = 0, bytes = 0;
                    char kind = 'i';
                    int consumed = 0;
                    if (std::sscanf(cursor, "%llx:%llx:%c%n", &address, &bytes, &kind, &consumed) < 3 || consumed == 0) break;
                    pass->listed.push_back({{address, bytes}, kind == 'o'});
                    cursor += consumed;
                }
            } else {
                continue;
            }
            pass->directory = directory;
            parsed->passes.push_back(std::move(pass));
        }
        std::fprintf(stderr, "[capture-pass] %zu passes to capture, budget %s\n", parsed->passes.size(), parsed->budget == ~std::uint64_t{0} ? "none" : (std::to_string(parsed->budget >> 20u) + " MiB").c_str());
        return parsed;
    }();
    return *plan;
}

// The accessible prefix of a guest range (whole 4 KiB pages; a range can run past the end of a mapping, as the depth
// and HTILE size estimates do).
std::uint64_t AccessibleBytes(std::uint64_t address, std::uint64_t bytes) {
    if (GuestMemory::Accessible(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), static_cast<std::size_t>(bytes))) return bytes;
    std::uint64_t good = 0;
    while (good < bytes) {
        const auto step = std::min<std::uint64_t>(bytes - good, 4096u - ((address + good) & 4095u));
        if (!GuestMemory::Accessible(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address + good)), static_cast<std::size_t>(step))) break;
        good += step;
    }
    return good;
}

// Saves a guest range under `directory` (PassFiles: a blob per distinct content, chunked above 64 MiB), new bytes
// counted against the budget. Returns the bytes saved (0 when inaccessible or over the budget).
std::uint64_t SaveRange(PassCapture::Pass& pass, const std::filesystem::path& directory, std::uint64_t address, std::uint64_t bytes) {
    auto& plan = Passes();
    bytes = AccessibleBytes(address, bytes);
    if (bytes == 0 || pass.overBudget) return 0;
    const std::string_view data(reinterpret_cast<const char*>(static_cast<std::uintptr_t>(address)), static_cast<std::size_t>(bytes));
    std::lock_guard lock(plan.blobMutex);
    const auto admit = [&](std::size_t size) {
        if (plan.written.load() + size > plan.budget) return false;
        plan.written += size;
        return true;
    };
    if (!PassFiles::SaveRange(directory, plan.blobs, address, data, admit)) {
        std::fprintf(stderr, "[capture-pass] %s: the budget is spent at 0x%llx+0x%llx; the pass stays incomplete\n", pass.directory.string().c_str(), static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes));
        pass.overBudget = true;
        return 0;
    }
    return bytes;
}

void AppendRange(const PassCapture::Pass& pass, std::uint64_t address, std::uint64_t bytes, char kind) {
    if (std::FILE* file = std::fopen((pass.directory / "ranges.txt").string().c_str(), "a")) {
        std::fprintf(file, "%llx %llx %c\n", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes), kind);
        std::fclose(file);
    }
}

}

bool PassCapture::Active() {
    return !Passes().passes.empty();
}

// Once the budget is spent no pass starts; one that started finishes (its later ranges may be left out).
bool Startable(const PassCapture::Pass& pass) {
    auto& plan = Passes();
    return pass.packets != 0 || plan.written.load() < plan.budget;
}

PassCapture::Pass* PassCapture::ForDispatch(std::uint64_t program, std::uint64_t instance) {
    for (const auto& pass : Passes().passes) {
        if (!pass->draws && pass->program == program && pass->first == instance) return Startable(*pass) ? pass.get() : nullptr;
    }
    return nullptr;
}

PassCapture::Pass* PassCapture::ForDraw(std::uint64_t serial) {
    for (const auto& pass : Passes().passes) {
        if (pass->draws && pass->first <= serial && serial <= pass->last) return Startable(*pass) ? pass.get() : nullptr;
    }
    return nullptr;
}

void PassCapture::SavePacket(Pass& pass, const QueueState& queue, std::uint32_t queueId, std::span<const std::uint32_t> packet, std::span<const std::shared_ptr<const ShaderSnapshot>> shaders) {
    std::lock_guard lock(pass.mutex);
    std::error_code error;
    const auto packets = pass.directory / "packets";
    std::filesystem::create_directories(packets, error);
    const auto index = pass.packets++;
    if (index == 0) {
        // A directory left from an earlier run (a file still open elsewhere kept it from being removed) starts over.
        std::ofstream(pass.directory / "packets.txt", std::ios::trunc);
        std::ofstream(pass.directory / "ranges.txt", std::ios::trunc);
        std::filesystem::remove(pass.directory / "done.txt", error);
    }
    const auto state = Driver::serializeQueueState(queue);
    std::ofstream(packets / (std::to_string(index) + ".state"), std::ios::binary).write(reinterpret_cast<const char*>(state.data()), static_cast<std::streamsize>(state.size()));
    std::ofstream(packets / (std::to_string(index) + ".pm4"), std::ios::binary).write(reinterpret_cast<const char*>(packet.data()), static_cast<std::streamsize>(packet.size_bytes()));
    if (std::FILE* file = std::fopen((pass.directory / "packets.txt").string().c_str(), "a")) {
        std::fprintf(file, "%u %x\n", index, queueId);
        std::fclose(file);
    }
    // shaders/<code address>.shader: code address, header address, type, code words, header bytes (u64 counts).
    const auto directory = pass.directory / "shaders";
    std::filesystem::create_directories(directory, error);
    for (const auto& snapshot : shaders) {
        if (snapshot == nullptr) continue;
        char name[40];
        std::snprintf(name, sizeof(name), "%llx.shader", static_cast<unsigned long long>(snapshot->codeAddress));
        const auto path = directory / name;
        if (std::filesystem::exists(path, error)) continue;
        std::ofstream file(path, std::ios::binary);
        const std::uint64_t words = snapshot->code.size(), headerBytes = snapshot->header.size();
        file.write(reinterpret_cast<const char*>(&snapshot->codeAddress), 8);
        file.write(reinterpret_cast<const char*>(&snapshot->headerAddress), 8);
        file.write(reinterpret_cast<const char*>(&snapshot->type), 1);
        file.write(reinterpret_cast<const char*>(&words), 8);
        file.write(reinterpret_cast<const char*>(&headerBytes), 8);
        file.write(reinterpret_cast<const char*>(snapshot->code.data()), static_cast<std::streamsize>(words * 4));
        file.write(reinterpret_cast<const char*>(snapshot->header.data()), static_cast<std::streamsize>(headerBytes));
    }
}

namespace {

// Moves what this thread recorded so far (an active RecordScope's list) into the pass (call with the pass locked):
// SaveBefore and Finish run inside the scope of the draw they save for.
void MergeRecorded(PassCapture::Pass& pass, std::vector<GuestMemory::CheckedRange>& ranges) {
    for (const auto& range : ranges) {
        auto known = std::find_if(pass.recorded.begin(), pass.recorded.end(), [&](const auto& entry) { return entry.first.first == range.address; });
        if (known == pass.recorded.end()) pass.recorded.push_back({{range.address, range.bytes}, range.writable});
        else {
            known->first.second = std::max(known->first.second, range.bytes);
            known->second = known->second || range.writable;
        }
    }
    ranges.clear();
}

// Saves the ranges the driver checked for the pass that are not saved yet (call with the pass locked): a written one
// becomes an output; at Finish (`late`) only read-only ones still hold what the pass read.
void SaveRecorded(PassCapture::Pass& pass, bool late) {
    if (auto* live = GuestMemory::RecordedRanges()) MergeRecorded(pass, *live);
    for (const auto& [range, writable] : pass.recorded) {
        if (pass.overBudget) break;
        const auto& [address, bytes] = range;
        if (address == 0 || bytes == 0 || (late && writable) || pass.savedBefore.contains(address)) continue;
        pass.savedBefore.insert(address);
        GuestMemory::FlushGpuWrites(address, static_cast<std::size_t>(AccessibleBytes(address, bytes)));
        const auto saved = SaveRange(pass, pass.directory / "before", address, bytes);
        if (saved == 0) continue;
        AppendRange(pass, address, saved, writable ? 'o' : 'i');
        if (writable) pass.outputs.push_back(range);
    }
}

}

PassCapture::RecordScope::RecordScope(Pass* pass) : pass(pass) {
    if (pass != nullptr) previous = std::exchange(GuestMemory::RecordedRanges(), &ranges);
}

PassCapture::RecordScope::~RecordScope() {
    if (pass == nullptr) return;
    GuestMemory::RecordedRanges() = previous;
    std::lock_guard lock(pass->mutex);
    MergeRecorded(*pass, ranges);
}

void PassCapture::SaveBefore(Pass& pass, std::span<const Range> reads, std::span<const Range> writes, std::span<const std::pair<std::uint64_t, std::span<const std::byte>>> captured) {
    std::lock_guard lock(pass.mutex);
    const auto before = pass.directory / "before";
    auto& plan = Passes();
    for (const auto& [address, bytes] : captured) {
        if (pass.overBudget) break;
        if (address == 0 || bytes.empty() || !pass.savedBefore.insert(address).second) continue;
        const std::string_view data(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        std::lock_guard blobLock(plan.blobMutex);
        const auto admit = [&](std::size_t size) {
            if (plan.written.load() + size > plan.budget) return false;
            plan.written += size;
            return true;
        };
        if (PassFiles::SaveRange(before, plan.blobs, address, data, admit)) AppendRange(pass, address, bytes.size(), 'i');
        else pass.overBudget = true;
    }
    for (const auto& [address, bytes] : reads) {
        if (pass.overBudget) break;
        if (address == 0 || bytes == 0 || !pass.savedBefore.insert(address).second) continue;
        GuestMemory::FlushGpuWrites(address, static_cast<std::size_t>(AccessibleBytes(address, bytes)));
        const auto saved = SaveRange(pass, before, address, bytes);
        if (saved != 0) AppendRange(pass, address, saved, std::any_of(writes.begin(), writes.end(), [&](const Range& write) { return write.first == address; }) ? 'o' : 'i');
    }
    for (const auto& write : writes) {
        if (write.first != 0 && write.second != 0 && std::none_of(pass.outputs.begin(), pass.outputs.end(), [&](const Range& known) { return known.first == write.first; })) pass.outputs.push_back(write);
    }
    SaveRecorded(pass, false);
}

void PassCapture::Finish(Pass& pass) {
    std::lock_guard lock(pass.mutex);
    if (pass.done) return;
    SaveRecorded(pass, true);
    std::size_t saved = 0;
    for (const auto& [address, bytes] : pass.outputs) {
        GuestMemory::FlushGpuWrites(address, static_cast<std::size_t>(AccessibleBytes(address, bytes)));
        saved += SaveRange(pass, pass.directory / "after", address, bytes) != 0;
    }
    pass.done = true;
    if (std::FILE* file = std::fopen((pass.directory / "done.txt").string().c_str(), "w")) {
        std::fprintf(file, "packets %u\nbefore %zu\nafter %zu of %zu\nover_budget %d\n", pass.packets, pass.savedBefore.size(), saved, pass.outputs.size(), pass.overBudget ? 1 : 0);
        std::fclose(file);
    }
    std::fprintf(stderr, "[capture-pass] %s: done, %u packets, %zu ranges before, %zu of %zu after%s\n", pass.directory.string().c_str(), pass.packets, pass.savedBefore.size(), saved, pass.outputs.size(), pass.overBudget ? ", over the budget" : "");
}

}
