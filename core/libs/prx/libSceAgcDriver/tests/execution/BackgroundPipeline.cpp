#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

alignas(256) std::array<std::uint32_t, 64> Input{};
alignas(256) std::array<std::uint32_t, 64> Output{};

// buffer_load_format_x v1 from s[0:3] (32-bit format, ADD_TID) and buffer_store_dword v1 at v0 * 4 through s[4:7].
alignas(256) constexpr std::array<std::uint32_t, 8> Code{0x7e100500u, 0x88030803u, 0x34040082u, 0xe0000000u, 0x80000100u, 0xe0701000u, 0x80010102u, 0xbf810000u};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t records) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), records, 0x31016facu};
}

bool Written(std::uint32_t threads) {
    for (std::uint32_t lane = 0; lane < threads; ++lane) {
        if (Output[lane] != Input[lane]) return false;
    }
    return true;
}

bool Untouched(std::uint32_t threads) {
    return std::all_of(Output.begin(), Output.begin() + threads, [](std::uint32_t word) { return word == 0xdeadbeefu; });
}

void Run(AgcDriver::VulkanDevice& device) {
    for (std::uint32_t index = 0; index < Input.size(); ++index) Input[index] = 0x1000u + index;
    const auto threads = std::min(32u, device.Target().subgroupSize);
    std::vector<std::uint32_t> userData(12, 0u);
    auto input = BufferDescriptor(Input.data(), threads);
    input[1] |= 4u << 16u;
    input[3] = 0x11800000u | (20u << 12u) | 4u;
    std::copy(input.begin(), input.end(), userData.begin());
    const auto output = BufferDescriptor(Output.data(), threads * 4u);
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(Code.data()), std::as_bytes(std::span(Code))}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{threads, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    ShaderRecompiler::RecompileRequest request{{ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(Code.data()), Code, 0, {}}, {32u, 0u, userData, compute, std::nullopt, std::nullopt, memory}, device.Target(), {0u, 0u, 0u, 128u}};
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    std::size_t empty = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (;;) {
        Output.fill(0xdeadbeefu);
        device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(Code.data()));
        device.WaitIdle();
        if (Written(threads)) break;
        Require(Untouched(threads), "a dispatch whose pipeline was still being created wrote part of its output");
        Require(std::chrono::steady_clock::now() < deadline, "the background pipeline was not ready after 20 s");
        ++empty;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Output.fill(0xdeadbeefu);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(Code.data()));
    device.WaitIdle();
    Require(Written(threads), "a dispatch after the background pipeline was ready did not write its output");
    std::printf("background pipeline: %zu dispatches did nothing before the pipeline was ready\n", empty);
    // A pipeline still being created when the device goes: its thread is joined in the teardown.
    auto other = request;
    std::vector<std::uint32_t> otherData(userData);
    otherData[11] = 1u;
    other.context.userData = otherData;
    const auto second = ShaderRecompiler::Recompile(other);
    if (second.PipelineVariantId() != result.PipelineVariantId()) device.Dispatch(second, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(Code.data()));
}

}

int main() {
    try {
#ifdef _WIN32
        _putenv_s("APS5_BACKGROUND_PIPELINE_WORDS", "1");
#else
        setenv("APS5_BACKGROUND_PIPELINE_WORDS", "1", 1);
#endif
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        GuestAllocations::Mutation().Add(Input.data(), sizeof(Input), true, true, true);
        GuestAllocations::Mutation().Add(Output.data(), sizeof(Output), true, true, true);
        struct Release {
            ~Release() {
                GuestAllocations::Mutation().Remove(Input.data());
                GuestAllocations::Mutation().Remove(Output.data());
            }
        } release;
        Run(*device);
        device.reset();
        std::puts("background pipeline tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
