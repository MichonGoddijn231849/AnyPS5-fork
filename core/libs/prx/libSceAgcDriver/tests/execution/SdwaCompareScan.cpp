#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
alignas(256) std::array<std::uint32_t, Threads * 2> Input{};
alignas(256) std::array<std::uint32_t, Threads * 3> Output{};

alignas(256) constexpr std::array<std::uint32_t, 53> Code{
    0x34060082, 0xe0301000, 0x80000003, 0xe0301080, 0x80000803, 0xbf8c3f70, 0xbeea03ff, 0x0000ffff,
    0x7c0200f9, 0x0686eb80, 0x7c0210f9, 0x06868f80, 0x020104f9, 0x86860680, 0xd5010001, 0x01ad0880,
    0xd5010002, 0x003d1080, 0xd7720000, 0x040a0300, 0x7d8a0080, 0x38020082, 0x020c0300, 0xbefe03c1,
    0xbeea03ff, 0x01010101, 0x876bff6a, 0x000000ff, 0xbeea407e, 0x02020c80, 0x380202fa, 0xff011101,
    0x380202fa, 0xff011201, 0x380202fa, 0xff011401, 0x380202fa, 0xff011801, 0xbefe036b, 0xd7600010,
    0x00011f01, 0xd7600011, 0x00013f01, 0x880f1110, 0xbefe03c1, 0x7e12020f, 0xe0701000, 0x80010603,
    0xe0701080, 0x80010903, 0xe0701100, 0x80010103, 0xbf810000,
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

void Expect(std::uint32_t lane, std::uint32_t actual, std::uint32_t expected, const char* name) {
    Require(actual == expected, std::string("sdwa compare scan: lane ") + std::to_string(lane) + " " + name + " is " + Hex(actual) + ", expected " + Hex(expected));
}

void Run(AgcDriver::VulkanDevice& device) {
    for (std::uint32_t lane = 0; lane < Threads; ++lane) {
        Input[lane] = std::bit_cast<std::uint32_t>(lane % 3u == 0u ? 1.0f : -1.0f);
        Input[Threads + lane] = std::bit_cast<std::uint32_t>(lane % 5u == 1u ? 2.0f : 0.0f);
    }
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(8, 0u);
    const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size() * 4u));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code(Code);
    const std::array<ShaderRecompiler::MemoryRegion, 2> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}, {reinterpret_cast<std::uintptr_t>(Output.data()), std::as_bytes(std::span(Output))}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

void Check() {
    std::array<std::uint32_t, Threads> classes{};
    std::uint32_t wave = 0;
    for (std::uint32_t lane = 0; lane < Threads; ++lane) {
        const std::uint32_t bits = (lane < 16u ? 2u : 0u) | (lane % 3u == 0u ? 4u : 0u) | (lane % 5u == 1u ? 8u : 0u);
        classes[lane] = bits != 0u ? bits | 2u : 0u;
        wave |= classes[lane];
    }
    for (std::uint32_t lane = 0; lane < Threads; ++lane) {
        std::uint32_t scan = 0;
        for (std::uint32_t source = lane & ~15u; source <= lane; ++source) scan |= classes[source];
        Expect(lane, Output[lane], classes[lane], "class");
        Expect(lane, Output[Threads + lane], wave, "wave class");
        Expect(lane, Output[2 * Threads + lane], scan, "row scan");
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (device->Target().subgroupSize < 32) {
            std::printf("skipped, subgroup size %u cannot hold a wave32\n", device->Target().subgroupSize);
            return VulkanTestSkipped;
        }
        Run(*device);
        Check();
        std::puts("sdwa compare scan tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
