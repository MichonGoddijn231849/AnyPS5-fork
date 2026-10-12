#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

alignas(256) std::array<std::uint32_t, 256> Input{};
alignas(256) std::array<std::uint32_t, 64> Output{};

alignas(256) constexpr std::array<std::uint32_t, 8> Code{0x7e100500u, 0x88030803u, 0x34040082u, 0xe0000000u, 0x80000100u, 0xe0701000u, 0x80010102u, 0xbf810000u};

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t records) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), records, 0x31016facu};
}

void Run(AgcDriver::VulkanDevice& device) {
    struct Case {
        std::uint32_t stride;
        std::uint32_t records;
        std::uint32_t format;
        std::uint32_t selector;
    };
    constexpr std::array<Case, 5> cases{{{4u, 32u, 20u, 4u}, {8u, 32u, 5u, 4u}, {8u, 32u, 11u, 4u}, {4u, 32u, 18u, 5u}, {4u, 16u, 20u, 4u}}};
    for (std::uint32_t index = 0; index < Input.size(); ++index) Input[index] = 0x3f000000u + index * 0x201u;
    const auto threads = std::min(32u, device.Target().subgroupSize);
    std::vector<std::uint32_t> userData(12, 0u);
    const auto output = BufferDescriptor(Output.data(), threads * 4u);
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(Code.data()), std::as_bytes(std::span(Code))}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{threads, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    for (const auto& value : cases) {
        auto input = BufferDescriptor(Input.data(), value.records);
        input[1] |= value.stride << 16u;
        input[3] = 0x11800000u | (value.format << 12u) | value.selector;
        std::copy(input.begin(), input.end(), userData.begin());
        Output.fill(0xdeadbeefu);
        ShaderRecompiler::RecompileRequest request{{ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(Code.data()), Code, 0, {}}, {32u, 0u, userData, compute, std::nullopt, std::nullopt, memory}, device.Target(), {0u, 0u, 0u, 128u}};
        request.useCache = false;
        const auto result = ShaderRecompiler::Recompile(request);
        device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(Code.data()));
        device.WaitIdle();
        for (std::uint32_t lane = 0; lane < threads; ++lane) {
            auto expected = lane >= value.records ? 0u : Input[lane * value.stride / 4u];
            if (value.format == 5u) expected &= 0xffu;
            if (value.format == 11u) expected &= 0xffffu;
            if (value.format == 18u) expected = (expected >> 8u) & 0xffu;
            Require(Output[lane] == expected, "computed V# formatted load: lane " + std::to_string(lane) + " format " + std::to_string(value.format) + " is " + Hex(Output[lane]) + ", expected " + Hex(expected));
        }
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
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
        std::puts("computed descriptor format tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
