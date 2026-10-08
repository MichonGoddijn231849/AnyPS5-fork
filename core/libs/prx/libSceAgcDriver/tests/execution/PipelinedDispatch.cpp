// Queue 0's compute dispatches among the pipelined commit items (docs/dev/perf/NIGHT_PROGRESS.md, B5). Each round submits
// through the DCB an immediate fill of an input buffer (a pipelined DMA_DATA, APS5_PIPELINE_DMA=1) and then a dispatch
// that copies the input into an output buffer with one added (buffer_load_dword, buffer_store_dword, one wave of 32).
// After the submission's wait the output must hold the value the fill before the dispatch stored, plus one, and the
// input the fill stored. The rounds use new values, so after the first the dispatch cache hits and reuses its recipe.
// The dispatch's pipeline drains (DISPATCH_DIRECT, opcode 0x15) are counted. Without APS5_PIPELINE_DISPATCH, every
// dispatch drains. With it, a dispatch is a commit item once its cache entry has a recipe: the first round of each part
// (no entry yet) drains, and the rest do not, including those whose input is still a pending fill (the device reads the
// imported input in commit order). The third part's dispatch reads its input with a scalar load from the input's address,
// which the capture reads on the CPU: its entry covers the input, so a pending fill of the input drains before the lookup
// (the read-side rule), and the output is checked against the value of each round's fill. Needs a Vulkan device (skipped otherwise).
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using AgcDriver::DriverDetail::DrawPipeline;

constexpr std::uint32_t Lanes = 32;
constexpr std::uint32_t BufferBytes = Lanes * 4u;
constexpr std::uint32_t DispatchOpcode = 0x15;

// Each buffer is a 64 KiB guest block of its own (as FlatD16Loads' GuestBlock, registered as guest memory): the device
// imports such buffers directly, which a dispatch recipe needs, and the dispatch's captured regions do not reach another
// buffer. The shader uses the first 128 bytes of each.
constexpr std::size_t BlockBytes = 65536;
std::uint32_t* NewBlock() {
#ifdef _WIN32
    auto* block = static_cast<std::uint32_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    auto* block = static_cast<std::uint32_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
    if (block == nullptr) throw std::runtime_error("pipelined dispatch: cannot allocate a guest block");
    std::memset(block, 0, BlockBytes);
    GuestAllocations::Mutation().Add(block, BlockBytes, true, true);
    return block;
}
std::uint32_t* Input = nullptr;
std::uint32_t* Output = nullptr;
std::uint32_t* Scratch = nullptr;

// v_lshlrev_b32 v1, 2, v0; buffer_load_dword v2, v1, s[0:3], 0 offen; s_waitcnt vmcnt(0); v_add_nc_u32 v3, 1, v2;
// buffer_store_dword v3, v1, s[4:7], 0 offen; s_endpgm (llvm-mc, gfx1030; the same encodings as BufferAtomics).
alignas(256) constexpr std::array<std::uint32_t, 8> Code{0x34020082u, 0xe0301000u, 0x80000201u, 0xbf8c3f70u, 0x4a060481u, 0xe0701000u, 0x80010301u, 0xbf810000u};

// s_load_dword s8, s[0:1], 0 (the first dword at the guest address in s[0:1], which is the input's address: the first two
// words of its V#); s_waitcnt lgkmcnt(0); v_lshlrev_b32 v1, 2, v0; v_mov_b32 v2, s8; v_add_nc_u32 v2, 1, v2;
// buffer_store_dword v2, v1, s[4:7], 0 offen; s_endpgm. A raw pointer, not a V#: a buffer V# read through the scalar unit
// is a device read that the capture does not take on the CPU, so its entry has no region over the input.
alignas(256) constexpr std::array<std::uint32_t, 9> ScalarCode{0xf4000200u, 0xfa000000u, 0xbf8cc07fu, 0x34020082u, 0x7e040208u, 0x4a040481u, 0xe0701000u, 0x80010201u, 0xbf810000u};

void check(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error("pipelined dispatch: " + what);
}

// DMA_DATA with an immediate source (see PipelinedDma.cpp).
std::array<std::uint32_t, 7> FillPacket(std::uint32_t value, const void* destination, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(destination);
    return {0xc0055000u, 2u << 29u, value, 0u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), bytes};
}

// A V# for a raw buffer (the descriptor BufferAtomics uses).
std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x01016facu};
}

// The fill of `destination` with `value`, then the compute state and a 1 x 1 x 1 dispatch of `code` (wave32, 32 threads)
// that reads Input and writes Output.
std::vector<std::uint32_t> Round(const void* destination, std::uint32_t value, const void* code) {
    const auto fill = FillPacket(value, destination, BufferBytes);
    std::vector<std::uint32_t> words(fill.begin(), fill.end());
    const auto codeAddress = reinterpret_cast<std::uintptr_t>(code);
    const auto input = BufferDescriptor(Input, BufferBytes);
    const auto output = BufferDescriptor(Output, BufferBytes);
    const std::vector<std::uint32_t> commands{
        0xc0027600u, 0x20cu, static_cast<std::uint32_t>(codeAddress >> 8u), static_cast<std::uint32_t>(codeAddress >> 40u), // COMPUTE_PGM_LO/HI
        0xc0037600u, 0x207u, Lanes, 1u, 1u,                                                                                 // COMPUTE_NUM_THREAD_X/Y/Z
        0xc0017600u, 0x213u, 8u << 1u,                                                                                       // COMPUTE_PGM_RSRC2: 8 user SGPRs
        0xc0087600u, 0x240u, input[0], input[1], input[2], input[3], output[0], output[1], output[2], output[3],              // COMPUTE_USER_DATA_0..7
        0xc0031500u, 1u, 1u, 1u, 0x8041u,                                                                                    // DISPATCH_DIRECT 1, 1, 1 (wave32)
    };
    words.insert(words.end(), commands.begin(), commands.end());
    return words;
}

void Submit(std::vector<std::uint32_t>& words) {
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    check(sceAgcDriverSubmitDcb(&packet) == 0, "submission failed");
}

}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_PIPELINED_DRAWS", "1");
    _putenv_s("APS5_PIPELINE_DMA", "1");
#else
    setenv("APS5_PIPELINED_DRAWS", "1", 1);
    setenv("APS5_PIPELINE_DMA", "1", 1);
#endif
    try {
        {
            const auto probe = OpenVulkanTestDevice();
            if (!probe) return VulkanTestSkipped;
        }
        const char* dispatchSwitch = std::getenv("APS5_PIPELINE_DISPATCH");
        const bool pipelinedDispatch = dispatchSwitch != nullptr && dispatchSwitch[0] != '\0' && dispatchSwitch[0] != '0';

        Shader shader{};
        shader.file_header = 0x34333231;
        shader.version = 0x18;
        shader.header_size = sizeof(Shader);
        shader.shader_size = sizeof(Code);
        shader.code = Code.data();
        AgcDriverRegisterShader_nid_postfix(&shader);
        Shader scalarShader{};
        scalarShader.file_header = 0x34333231;
        scalarShader.version = 0x18;
        scalarShader.header_size = sizeof(Shader);
        scalarShader.shader_size = sizeof(ScalarCode);
        scalarShader.code = ScalarCode.data();
        AgcDriverRegisterShader_nid_postfix(&scalarShader);

        Input = NewBlock();
        Output = NewBlock();
        Scratch = NewBlock();
        // Part 1: the fill is of the dispatch's input, which may still be pending when the dispatch is handled.
        const std::array<std::uint32_t, 5> values{0x0a0b0c0du, 0x11112222u, 0x33334444u, 0x55556666u, 0x77778888u};
        for (std::size_t round = 0; round < values.size(); ++round) {
            const auto value = values[round];
            const auto drainsBefore = DrawPipeline::DrainRequestsByOpcode(DispatchOpcode);
            auto words = Round(Input, value, Code.data());
            Submit(words);
            AgcDriverWaitIdle_nid_postfix();
            // The output is written on the GPU; code that reads guest memory directly flushes the deferred results first.
            AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(Output), BufferBytes);
            const auto drains = DrawPipeline::DrainRequestsByOpcode(DispatchOpcode) - drainsBefore;
            for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
                check(Input[lane] == value, "the immediate fill did not land in lane " + std::to_string(lane));
                check(Output[lane] == value + 1u, "round " + std::to_string(round) + ": lane " + std::to_string(lane) + " of the output is " + std::to_string(Output[lane]) + ", expected " + std::to_string(value + 1u) + " (the dispatch did not see the fill before it)");
            }
            std::printf("pipelined dispatch: input fill round %zu, fill 0x%08x, dispatch drains %llu\n", round, value, static_cast<unsigned long long>(drains));
            if (!pipelinedDispatch) check(drains >= 1, "a dispatch did not drain the pipeline, and APS5_PIPELINE_DISPATCH is off");
            if (pipelinedDispatch) check(drains <= 1, "a dispatch drained the pipeline more than once (" + std::to_string(drains) + ")");
        }
        // Part 2: the fill is of the scratch buffer, which the dispatch does not read. The output is still the input plus one.
        for (std::size_t round = 0; round < 3; ++round) {
            const auto value = 0xc0de0000u + static_cast<std::uint32_t>(round);
            const auto drainsBefore = DrawPipeline::DrainRequestsByOpcode(DispatchOpcode);
            auto words = Round(Scratch, value, Code.data());
            Submit(words);
            AgcDriverWaitIdle_nid_postfix();
            AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(Scratch), BufferBytes);
            AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(Output), BufferBytes);
            const auto drains = DrawPipeline::DrainRequestsByOpcode(DispatchOpcode) - drainsBefore;
            for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
                check(Scratch[lane] == value, "the scratch fill did not land in lane " + std::to_string(lane));
                check(Output[lane] == Input[lane] + 1u, "scratch round " + std::to_string(round) + ": lane " + std::to_string(lane) + " of the output is " + std::to_string(Output[lane]) + ", expected " + std::to_string(Input[lane] + 1u));
            }
            std::printf("pipelined dispatch: scratch fill round %zu, fill 0x%08x, dispatch drains %llu\n", round, value, static_cast<unsigned long long>(drains));
            if (!pipelinedDispatch) check(drains >= 1, "a dispatch did not drain the pipeline, and APS5_PIPELINE_DISPATCH is off");
            if (pipelinedDispatch) check(drains == 0, "a dispatch that reads nothing pending drained the pipeline (" + std::to_string(drains) + ")");
        }
        // Part 3: the dispatch reads its input through a scalar load, which the capture reads on the CPU, so the entry's
        // regions cover the input. Each round's fill is of a new value, and a pending fill of the input must drain before
        // the lookup (the read-side rule): with the value the entry has from an earlier round, the output would be stale.
        const std::array<std::uint32_t, 5> scalarValues{0x13572468u, 0x2468ace0u, 0x0f1e2d3cu, 0x9abcdef0u, 0x31415926u};
        for (std::size_t round = 0; round < scalarValues.size(); ++round) {
            const auto value = scalarValues[round];
            const auto drainsBefore = DrawPipeline::DrainRequestsByOpcode(DispatchOpcode);
            auto words = Round(Input, value, ScalarCode.data());
            Submit(words);
            AgcDriverWaitIdle_nid_postfix();
            AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(Output), BufferBytes);
            const auto drains = DrawPipeline::DrainRequestsByOpcode(DispatchOpcode) - drainsBefore;
            for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
                check(Output[lane] == value + 1u, "scalar round " + std::to_string(round) + ": lane " + std::to_string(lane) + " of the output is " + std::to_string(Output[lane]) + ", expected " + std::to_string(value + 1u) + " (a stale input)");
            }
            std::printf("pipelined dispatch: scalar read round %zu, fill 0x%08x, dispatch drains %llu\n", round, value, static_cast<unsigned long long>(drains));
            if (!pipelinedDispatch) check(drains >= 1, "a dispatch did not drain the pipeline, and APS5_PIPELINE_DISPATCH is off");
            if (pipelinedDispatch) check(drains <= 1, "a dispatch drained the pipeline more than once (" + std::to_string(drains) + ")");
        }
        LibcRunShutdown_nid_postfix();
        std::puts("pipelined dispatch tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { LibcRunShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
