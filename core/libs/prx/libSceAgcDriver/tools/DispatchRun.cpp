// agc_dispatch_run <shader_*.req> --groups X,Y,Z [--zero ADDR:BYTES]... [--poison ADDR:BYTES]... [--dump ADDR:BYTES]...
// Runs one compute dispatch the driver saved with APS5_DUMP_SHADERS on the local Vulkan device, without the
// game or a capture: the request's memory regions are mapped at their guest addresses, --zero and --poison map
// further ranges (zeros, or 0xa5 bytes, so a write shows) for the resources the request does not hold, and each
// --dump range is saved after the dispatch as dispatch_<addr>.bin with the count of bytes that changed.
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Capture/PassFiles.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "ControlFlow/RequestSerializer.hpp"
#include "Recompiler.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Range {
    std::uint64_t address;
    std::uint64_t bytes;
};

Range ParseRange(const std::string& text) {
    const auto colon = text.find(':');
    if (colon == std::string::npos) throw std::runtime_error("a range is <hex address>:<hex bytes>, not " + text);
    return {std::stoull(text.substr(0, colon), nullptr, 16), std::stoull(text.substr(colon + 1), nullptr, 16)};
}

std::string ReadText(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error(std::string("cannot read ") + path);
    std::ostringstream stream;
    stream << file.rdbuf();
    auto text = stream.str();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text;
}

// Every range rounded out to 64 KiB and merged, then committed at its address.
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
#ifdef _WIN32
        auto* pointer = reinterpret_cast<void*>(static_cast<std::uintptr_t>(range.address));
        void* base = nullptr;
        if (GuestArena::GuestArenaContains_nid_postfix(pointer, static_cast<std::size_t>(range.bytes))) {
            // Inside the guest arena libc reserved at load (placeholders): committed through it.
            GuestArena::GuestArenaCommit_nid_postfix(pointer, static_cast<std::size_t>(range.bytes), PAGE_READWRITE, 65536);
            GuestArena::GuestArenaMarkUsed_nid_postfix(pointer, static_cast<std::size_t>(range.bytes));
            base = pointer;
        } else {
            base = VirtualAlloc(pointer, static_cast<SIZE_T>(range.bytes), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        }
#else
        void* base = nullptr;
#endif
        if (base != reinterpret_cast<void*>(static_cast<std::uintptr_t>(range.address))) {
            // Taken in this process (the game could not read it either, or a DLL sits there): left unmapped.
            MEMORY_BASIC_INFORMATION info{};
            VirtualQuery(reinterpret_cast<void*>(static_cast<std::uintptr_t>(range.address)), &info, sizeof(info));
            std::printf("cannot map 0x%llx+0x%llx at its guest address (error %lu; state 0x%lx base %p size 0x%zx); left unmapped\n", static_cast<unsigned long long>(range.address), static_cast<unsigned long long>(range.bytes), GetLastError(), info.State, info.AllocationBase, info.RegionSize);
            if (base != nullptr) VirtualFree(base, 0, MEM_RELEASE);
            continue;
        }
        GuestAllocations::Mutation().Add(base, static_cast<std::size_t>(range.bytes), true, true, true);
        done.push_back(range);
    }
    return done;
}

}

int main(int argc, char** argv) {
    try {
        const char* path = nullptr;
        std::uint32_t groups[3] = {1, 1, 1};
        std::vector<Range> zero, poison, dump;
        std::vector<std::pair<Range, std::filesystem::path>> inputs;
        bool useCache = false;
        // --compile-only [optimized|unoptimized]: recompile and time the host driver's pipeline compile, nothing else.
        std::string compileOnly;
        std::filesystem::path blobs;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            const auto value = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error(argument + " needs a value");
                return argv[++i];
            };
            if (argument == "--groups") {
                const auto text = value();
                if (std::sscanf(text.c_str(), "%u,%u,%u", &groups[0], &groups[1], &groups[2]) != 3) throw std::runtime_error("--groups takes X,Y,Z");
            } else if (argument == "--zero") {
                zero.push_back(ParseRange(value()));
            } else if (argument == "--poison") {
                poison.push_back(ParseRange(value()));
            } else if (argument == "--dump") {
                dump.push_back(ParseRange(value()));
            } else if (argument == "--compile-only") {
                compileOnly = value();
            } else if (argument == "--compile-dump") {
                // A pipeline APS5_DUMP_PIPELINE_DIR wrote: created into the pipeline cache (ANYPS5_SHADER_CACHE_DIR),
                // saved when the device goes.
                const auto dumped = value();
                {
                    AgcDriver::VulkanDevice device;
                    std::printf("compiling %s\n", dumped.c_str());
                    std::fflush(stdout);
                    std::printf("pipeline created in %.1f s\n", device.CompileDumpedPipeline(dumped));
                }
                return 0;
            } else if (argument == "--use-cache") {
                // The recompile goes through the shader disk cache (ANYPS5_SHADER_CACHE_DIR), as in the driver.
                useCache = true;
            } else if (argument == "--inputs") {
                // inputs_<program>/<address>.bin files from APS5_CAPTURE_INPUTS: the ranges as the game had them.
                // APS5_CAPTURE_PASSES' before/ directories also hold large ranges as chunk manifests (PassFiles).
                for (const auto& entry : std::filesystem::directory_iterator(value())) {
                    const auto extension = entry.path().extension();
                    if (extension == ".bin") {
                        inputs.push_back({{std::stoull(entry.path().stem().string(), nullptr, 16), static_cast<std::uint64_t>(entry.file_size())}, entry.path()});
                    } else if (extension == ".chunks") {
                        std::ifstream manifest(entry.path());
                        std::string word, size;
                        manifest >> word >> size;
                        inputs.push_back({{std::stoull(entry.path().stem().string(), nullptr, 16), std::stoull(size, nullptr, 16)}, entry.path()});
                    }
                }
            } else if (argument == "--blobs") {
                // Where dumps above PassFiles::ChunkThreshold are stored as chunks (a <address>.chunks manifest here).
                blobs = value();
            } else {
                path = argv[i];
            }
        }
        if (path == nullptr) throw std::runtime_error("usage: agc_dispatch_run <shader_*.req> --groups X,Y,Z [--zero A:B]... [--poison A:B]... [--dump A:B]...");
        auto deserialized = ShaderRecompiler::RequestSerializer{}.Deserialize(ReadText(path));
        auto& request = deserialized.request;
        if (!request.context.compute.has_value()) throw std::runtime_error("the request is not a compute shader");
        if (!compileOnly.empty()) {
            AgcDriver::VulkanDevice device;
            request.target = device.Target();
            auto result = ShaderRecompiler::Recompile(request);
            // APS5_COMPILE_SALT=<n> raises the module's id bound by n: the same program under another hash, so the host driver's own shader cache misses.
            if (const char* salt = std::getenv("APS5_COMPILE_SALT")) result.spirv[3] += static_cast<std::uint32_t>(std::strtoul(salt, nullptr, 10));
            const bool optimize = compileOnly == "optimized";
            std::printf("recompiled %zu SPIR-V words; compiling the pipeline %s\n", result.spirv.size(), optimize ? "optimized" : "unoptimized");
            std::fflush(stdout);
            const auto privateMb = [] {
#ifdef _WIN32
                PROCESS_MEMORY_COUNTERS_EX counters{};
                counters.cb = sizeof(counters);
                K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters));
                return static_cast<double>(counters.PrivateUsage) / 1048576.0;
#else
                return 0.0;
#endif
            };
            const auto before = privateMb();
            const auto seconds = device.CompileComputePipeline(result, optimize);
            std::printf("pipeline compiled in %.1f s; process private %.0f MiB before, %.0f MiB after the pipeline was destroyed\n", seconds, before, privateMb());
            return 0;
        }
        std::vector<Range> all = zero;
        all.insert(all.end(), poison.begin(), poison.end());
        all.insert(all.end(), dump.begin(), dump.end());
        for (const auto& input : inputs) all.push_back(input.first);
        for (const auto& region : request.context.memory) all.push_back({region.guestAddress, region.bytes.size()});
        const auto mapped = MapRanges(all);
        std::printf("mapped %zu ranges for %zu memory regions\n", mapped.size(), request.context.memory.size());
        const auto mappedAt = [&](std::uint64_t address, std::uint64_t bytes) { return std::any_of(mapped.begin(), mapped.end(), [&](const Range& range) { return range.address <= address && address + bytes <= range.address + range.bytes; }); };
        std::erase_if(poison, [&](const Range& range) { return !mappedAt(range.address, range.bytes); });
        std::erase_if(dump, [&](const Range& range) { return !mappedAt(range.address, range.bytes); });
        std::size_t loaded = 0;
        for (const auto& [range, file] : inputs) {
            if (!mappedAt(range.address, range.bytes)) continue;
            loaded += AgcDriver::PassFiles::LoadRange(file.parent_path(), range.address, reinterpret_cast<void*>(static_cast<std::uintptr_t>(range.address)), static_cast<std::size_t>(range.bytes)) != 0;
        }
        if (!inputs.empty()) std::printf("loaded %zu of %zu captured input ranges\n", loaded, inputs.size());
        for (const auto& range : poison) std::memset(reinterpret_cast<void*>(static_cast<std::uintptr_t>(range.address)), 0xa5, static_cast<std::size_t>(range.bytes));
        for (const auto& region : request.context.memory) if (mappedAt(region.guestAddress, region.bytes.size())) std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(region.guestAddress)), region.bytes.data(), region.bytes.size());
        std::vector<std::vector<std::uint8_t>> before;
        for (const auto& range : dump) {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(range.address));
            before.emplace_back(bytes, bytes + range.bytes);
        }
        AgcDriver::VulkanDevice device;
        request.target = device.Target();
        request.useCache = useCache;
        const auto result = ShaderRecompiler::Recompile(request);
        std::printf("recompiled %zu SPIR-V words; dispatching %ux%ux%u groups\n", result.spirv.size(), groups[0], groups[1], groups[2]);
        device.Dispatch(result, groups[0], groups[1], groups[2], {}, request.shader.codeAddress);
        device.WaitIdle();
        for (const auto& range : dump) AgcDriver::Graphics::StorageTexture::FlushPending(range.address, static_cast<std::size_t>(range.bytes), nullptr, "dispatch run");
        device.WaitIdle();
        for (std::size_t index = 0; index < dump.size(); ++index) {
            const auto& range = dump[index];
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(range.address));
            std::size_t changed = 0;
            for (std::uint64_t at = 0; at < range.bytes; ++at) changed += bytes[at] != before[index][at];
            char name[64];
            std::snprintf(name, sizeof(name), "dispatch_%llx.bin", static_cast<unsigned long long>(range.address));
            if (range.bytes > AgcDriver::PassFiles::ChunkThreshold && !blobs.empty()) {
                AgcDriver::PassFiles::SaveRange(".", blobs, range.address, std::string_view(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(range.bytes)), [](std::size_t) { return true; });
                std::snprintf(name, sizeof(name), "%llx.chunks", static_cast<unsigned long long>(range.address));
            } else {
                std::ofstream(name, std::ios::binary).write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(range.bytes));
            }
            std::printf("0x%llx+0x%llx: %zu of %llu bytes changed -> %s\n", static_cast<unsigned long long>(range.address), static_cast<unsigned long long>(range.bytes), changed, static_cast<unsigned long long>(range.bytes), name);
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "agc_dispatch_run: %s\n", error.what());
        return 1;
    }
}
