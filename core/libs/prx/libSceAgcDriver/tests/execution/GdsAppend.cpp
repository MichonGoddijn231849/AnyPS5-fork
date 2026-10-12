#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Results = 3;
constexpr std::uint32_t GdsBase = 0x20;
constexpr std::uint32_t M0 = (GdsBase << 16u) | 0x40u;
alignas(256) std::array<std::uint32_t, Threads * Results> Output{};

alignas(256) constexpr std::array<std::uint32_t, 24> Code{
    0xbefc0308, 0x34060082, 0x7e0402c1,
    0xd8fa0000, 0x01000000, 0xd8f60008, 0x04000000,
    0x7d88008a, 0xbe893c6a, 0xd8fa0004, 0x02000000, 0xbefe0309, 0x7e0a0283, 0x7e0e0280, 0xd802000c, 0x00000507,
    0xbf8cc07f, 0xe0701000, 0x80010103, 0xe0701080, 0x80010203, 0xe0701100, 0x80010403,
    0xbf810000,
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

void Expect(std::uint32_t actual, std::uint32_t expected, const std::string& name) {
    Require(actual == expected, "gds append: " + name + " is " + Hex(actual) + ", expected " + Hex(expected));
}

std::array<std::uint32_t, 4> Counters() {
    std::array<std::uint32_t, 4> counters{};
    AgcDriver::GuestMemory::Read(AgcDriver::Pm4::GdsAddress() + GdsBase, std::as_writable_bytes(std::span(counters)));
    return counters;
}

void Run(AgcDriver::VulkanDevice& device) {
    const std::array<std::uint32_t, 4> initial{100u, 7u, 100u, 5u};
    AgcDriver::GuestMemory::Write(AgcDriver::Pm4::GdsAddress() + GdsBase, std::as_bytes(std::span(initial)));
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(9, 0u);
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    userData[8] = M0;
    const std::span<const std::uint32_t> code(Code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
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
    const auto first = Counters();
    Expect(first[0], 132u, "append counter after one dispatch");
    Expect(first[1], 17u, "partial-exec append counter after one dispatch");
    Expect(first[2], 68u, "consume counter after one dispatch");
    Expect(first[3], 101u, "ds_add_u32 counter after one dispatch");
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
    const auto second = Counters();
    Expect(second[0], 164u, "append counter after two dispatches");
    Expect(second[1], 27u, "partial-exec append counter after two dispatches");
    Expect(second[2], 36u, "consume counter after two dispatches");
    Expect(second[3], 197u, "ds_add_u32 counter after two dispatches");
    for (std::uint32_t lane = 0; lane < Threads; ++lane) {
        const auto name = "lane " + std::to_string(lane) + " ";
        Expect(Output[lane], 132u, name + "ds_append result");
        Expect(Output[Threads + lane], lane < 10u ? 17u : 0xffffffffu, name + "partial-exec ds_append result");
        Expect(Output[2 * Threads + lane], 68u, name + "ds_consume result");
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
        std::puts("gds append tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
