// agc_pass_run <pass directory> [--out DIR] [--blobs DIR]
// Runs one pass that APS5_CAPTURE_PASSES saved (see Execution/include/Driver/Capture/PassCapture.hpp) alone, through
// the whole driver, in a fresh process: its ranges are mapped at their guest addresses with the contents they had
// before the pass, its programs are registered, and each of its packets is submitted on queue 0 after restoring the
// queue state it ran in. Each output range is then saved as <out>/<address>.bin (default <pass>/isolated) for
// tools/fidelity to compare with what the frame replay left there.
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Capture/PassFiles.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Capture/Replay.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Range {
    std::uint64_t address;
    std::uint64_t bytes;
};

std::vector<std::byte> ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot read " + path.string());
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::byte> result(bytes.size());
    std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
}

// Every range rounded out to 64 KiB and merged, then committed at its address (as agc_dispatch_run does).
std::vector<Range> MapRanges(std::vector<Range> ranges) {
    constexpr std::uint64_t granule = 65536;
    for (auto& range : ranges) {
        const auto end = (range.address + range.bytes + granule - 1) & ~(granule - 1);
        range.address &= ~(granule - 1);
        range.bytes = end - range.address;
    }
    std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.address < b.address; });
    std::vector<Range> merged;
    for (const auto& range : ranges) {
        if (!merged.empty() && range.address <= merged.back().address + merged.back().bytes) {
            merged.back().bytes = std::max(merged.back().address + merged.back().bytes, range.address + range.bytes) - merged.back().address;
        } else {
            merged.push_back(range);
        }
    }
    std::vector<Range> done;
    for (const auto& range : merged) {
        auto* pointer = reinterpret_cast<void*>(static_cast<std::uintptr_t>(range.address));
        void* base = nullptr;
        if (GuestArena::GuestArenaContains_nid_postfix(pointer, static_cast<std::size_t>(range.bytes))) {
            GuestArena::GuestArenaCommit_nid_postfix(pointer, static_cast<std::size_t>(range.bytes), PAGE_READWRITE, 65536);
            GuestArena::GuestArenaMarkUsed_nid_postfix(pointer, static_cast<std::size_t>(range.bytes));
            base = pointer;
        } else {
            base = VirtualAlloc(pointer, static_cast<SIZE_T>(range.bytes), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        }
        if (base != pointer) {
            std::printf("cannot map 0x%llx+0x%llx at its guest address (error %lu); left unmapped\n", static_cast<unsigned long long>(range.address), static_cast<unsigned long long>(range.bytes), GetLastError());
            if (base != nullptr) VirtualFree(base, 0, MEM_RELEASE);
            continue;
        }
        GuestAllocations::Mutation().Add(base, static_cast<std::size_t>(range.bytes), true, true, true);
        done.push_back(range);
    }
    return done;
}

struct SavedShader {
    std::uint64_t codeAddress = 0;
    std::uint64_t headerAddress = 0;
    std::uint8_t type = 0;
    std::vector<std::uint32_t> code;
    std::vector<std::byte> header;
};

SavedShader ReadShader(const std::filesystem::path& path) {
    const auto bytes = ReadFile(path);
    SavedShader shader;
    std::size_t at = 0;
    const auto take = [&](void* destination, std::size_t size) {
        if (at + size > bytes.size()) throw std::runtime_error("truncated shader file " + path.string());
        std::memcpy(destination, bytes.data() + at, size);
        at += size;
    };
    std::uint64_t words = 0, headerBytes = 0;
    take(&shader.codeAddress, 8);
    take(&shader.headerAddress, 8);
    take(&shader.type, 1);
    take(&words, 8);
    take(&headerBytes, 8);
    shader.code.resize(static_cast<std::size_t>(words));
    take(shader.code.data(), static_cast<std::size_t>(words * 4));
    shader.header.resize(static_cast<std::size_t>(headerBytes));
    take(shader.header.data(), static_cast<std::size_t>(headerBytes));
    return shader;
}

}

int main(int argc, char** argv) {
    try {
        if (argc < 2) throw std::runtime_error("usage: agc_pass_run <pass directory> [--out DIR]");
        const std::filesystem::path pass = argv[1];
        std::filesystem::path out = pass / "isolated";
        // Outputs above PassFiles::ChunkThreshold are saved as chunk manifests into the capture's blob store.
        std::filesystem::path blobs = pass.parent_path().parent_path() / "capture-passes.txt.blobs";
        for (int i = 2; i < argc; ++i) {
            if (std::string(argv[i]) == "--out" && i + 1 < argc) out = argv[++i];
            else if (std::string(argv[i]) == "--blobs" && i + 1 < argc) blobs = argv[++i];
            else throw std::runtime_error(std::string("unknown argument ") + argv[i]);
        }
        // ranges.txt: "<address> <bytes> <i|o>" for every range saved under before/.
        std::vector<Range> inputs, outputs;
        {
            std::ifstream list(pass / "ranges.txt");
            if (!list) throw std::runtime_error("no ranges.txt in " + pass.string());
            std::string address, bytes, kind;
            while (list >> address >> bytes >> kind) {
                const Range range{std::stoull(address, nullptr, 16), std::stoull(bytes, nullptr, 16)};
                inputs.push_back(range);
                if (kind == "o") outputs.push_back(range);
            }
        }
        std::vector<SavedShader> shaders;
        if (std::filesystem::exists(pass / "shaders")) {
            for (const auto& entry : std::filesystem::directory_iterator(pass / "shaders")) shaders.push_back(ReadShader(entry.path()));
        }
        std::vector<Range> all = inputs;
        for (const auto& shader : shaders) {
            all.push_back({shader.codeAddress, shader.code.size() * 4});
            if (shader.headerAddress != 0 && !shader.header.empty()) all.push_back({shader.headerAddress, shader.header.size()});
        }
        const auto mapped = MapRanges(all);
        const auto mappedAt = [&](std::uint64_t address, std::uint64_t bytes) { return std::any_of(mapped.begin(), mapped.end(), [&](const Range& range) { return range.address <= address && address + bytes <= range.address + range.bytes; }); };
        std::size_t loaded = 0;
        for (const auto& range : inputs) {
            if (!mappedAt(range.address, range.bytes)) continue;
            loaded += AgcDriver::PassFiles::LoadRange(pass / "before", range.address, reinterpret_cast<void*>(static_cast<std::uintptr_t>(range.address)), static_cast<std::size_t>(range.bytes)) != 0;
        }
        for (const auto& shader : shaders) {
            if (mappedAt(shader.codeAddress, shader.code.size() * 4)) std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(shader.codeAddress)), shader.code.data(), shader.code.size() * 4);
            if (shader.headerAddress != 0 && mappedAt(shader.headerAddress, shader.header.size())) std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(shader.headerAddress)), shader.header.data(), shader.header.size());
            AgcDriver::Capture::ReplayRegisterShader(shader.codeAddress, shader.headerAddress, shader.type, shader.code, shader.header);
        }
        std::printf("mapped %zu ranges; loaded %zu of %zu inputs; registered %zu programs\n", mapped.size(), loaded, inputs.size(), shaders.size());
        // As the frame replay does after a restore: the loads are collected now, so the driver does not later take them
        // for CPU writes made while the pass's GPU results were pending (and keep the CPU's blocks over its results).
        std::vector<std::pair<std::uint64_t, std::uint64_t>> collected;
        for (const auto& range : mapped) collected.emplace_back(range.address, range.bytes);
        AgcDriver::Capture::ReplayCollectWrites(collected);

        // packets.txt: "<index> <queue>" in the order the pass ran them.
        std::ifstream packets(pass / "packets.txt");
        if (!packets) throw std::runtime_error("no packets.txt in " + pass.string());
        std::size_t submitted = 0;
        for (std::string index, queue; packets >> index >> queue;) {
            const auto state = ReadFile(pass / "packets" / (index + ".state"));
            const auto bytes = ReadFile(pass / "packets" / (index + ".pm4"));
            std::vector<std::uint32_t> words(bytes.size() / 4);
            std::memcpy(words.data(), bytes.data(), words.size() * 4);
            AgcDriverWaitIdle_nid_postfix();
            AgcDriver::Capture::ReplayRestoreQueueState(0, state);
            std::uint32_t* address = words.data();
            const auto size = static_cast<std::uint32_t>(words.size());
            if (AgcDriver::SubmitMany(&address, &size, 1, 0) != 0) throw std::runtime_error("packet " + index + " was refused");
            ++submitted;
        }
        AgcDriverWaitIdle_nid_postfix();
        // Storage images and resident targets keep their results on the GPU until flushed (as agc_dispatch_run does).
        for (const auto& range : outputs) {
            AgcDriver::Graphics::StorageTexture::FlushPending(range.address, static_cast<std::size_t>(range.bytes), nullptr, "pass run");
            AgcDriver::GuestMemory::FlushGpuWrites(range.address, static_cast<std::size_t>(range.bytes));
        }
        AgcDriverWaitIdle_nid_postfix();
        std::filesystem::create_directories(out);
        std::size_t saved = 0;
        for (const auto& range : outputs) {
            if (!mappedAt(range.address, range.bytes)) continue;
            const std::string_view data(reinterpret_cast<const char*>(static_cast<std::uintptr_t>(range.address)), static_cast<std::size_t>(range.bytes));
            if (range.bytes <= AgcDriver::PassFiles::ChunkThreshold) {
                std::ofstream(out / AgcDriver::PassFiles::RangeName(range.address, false), std::ios::binary).write(data.data(), static_cast<std::streamsize>(data.size()));
            } else {
                AgcDriver::PassFiles::SaveRange(out, blobs, range.address, data, [](std::size_t) { return true; });
            }
            ++saved;
        }
        std::printf("submitted %zu packets; saved %zu of %zu outputs into %s\n", submitted, saved, outputs.size(), out.string().c_str());
        // As agc_frame_replay leaves (ReplayPlatform::Exit): the outputs are written, and ExitProcess hangs in the DLL
        // detach of the GPU driver with the queue workers still running.
        std::fflush(nullptr);
        TerminateProcess(GetCurrentProcess(), 0);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "agc_pass_run: %s\n", error.what());
        std::fflush(nullptr);
        TerminateProcess(GetCurrentProcess(), 1);
        return 1;
    }
}
